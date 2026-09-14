#include "bridge.h"

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
 * Evergram <-> IRC mapping.
 *
 * Peers are addressed by a generated nick derived from their identity key, so a
 * client can forget about "1:rABC..." and just use a name. The full identity is
 * always available through WHOIS, and a client may also address a peer by its
 * identity key directly (it contains a ':' and is unambiguous).
 *
 * Everything shared between the two threads is in this struct and guarded by
 * `lock`. The SDK is only ever touched by the gateway thread.
 */

#define PEER_MAX 256
#define REQUEST_MAX 64
#define OUTGOING_MAX 512
#define SNAPSHOT_CHATS 64
#define SNAPSHOT_PARTICIPANTS 32
/* How much of the address a nick keeps. XRPL addresses run to 35 characters and
 * an IRC nick is capped around 31, so it is a prefix; /whois shows the whole
 * identity. Collisions are resolved by remember_peer_locked(). */
#define NICK_BODY 20

typedef enum {
    REQ_SEND = 0,      /* send text, creating a one-on-one first if needed */
    REQ_LEAVE,         /* leave a group */
    REQ_JOIN_CODE,     /* resolve an invite code and ask to join */
    REQ_ADD,           /* add a participant */
    REQ_REMOVE,        /* remove a participant */
    REQ_ACCEPT_CHAT,   /* accept a chat request */
    REQ_DECLINE_CHAT,  /* decline a chat request */
    REQ_ACCEPT_INVITE, /* accept a group invite */
    REQ_DENY_JOIN,     /* deny a join request */
    REQ_REFRESH,       /* re-read the chat list */
    REQ_PROFILE,       /* fetch a peer's profile, for its nickname */
    REQ_SET_NICK,      /* /nick: rename the Evergram profile */
} request_kind_t;

typedef struct {
    request_kind_t kind;
    char a[EVERGRAM_IDENTITY_SIZE]; /* identity, or chat id, depending on kind */
    char b[EVERGRAM_CHAT_ID_SIZE];  /* chat id for the two-argument kinds */
    char code[EVERGRAM_INVITE_CODE_SIZE];
    char text[IRC_TEXT_MAX];
    char origin[IRC_NICK_MAX]; /* who to answer; "" broadcasts */
    bool create_target;        /* REQ_SEND: open the chat first */
} request_t;

typedef struct {
    char target[IRC_NICK_MAX]; /* nick, or "*" for every client */
    char line[IRC_LINE_MAX];
} outgoing_t;

typedef struct {
    char chat_id[EVERGRAM_CHAT_ID_SIZE];
    char type[16];
    char name[EVERGRAM_NAME_SIZE];
    size_t participant_count;
    char participants[SNAPSHOT_PARTICIPANTS][EVERGRAM_IDENTITY_SIZE];
} snapshot_chat_t;

typedef struct {
    char identity[EVERGRAM_IDENTITY_SIZE];
    /* The nick the client sees: the Evergram profile nickname when the peer has
     * one, the address prefix otherwise. The identity stays the key. */
    char nick[IRC_NICK_MAX];
    char profile_nickname[EVERGRAM_NICKNAME_SIZE];
    bool profile_requested; /* so one lookup is queued per peer, not per event */
    /* The last chat this peer wrote in. A reply must go back into exactly that
     * conversation: creating a "matching" one instead would deliver it
     * somewhere the peer is not looking. */
    char last_chat_id[EVERGRAM_CHAT_ID_SIZE];
} peer_t;

struct bridge {
    irc_server_t *server;
    evergram_bot_t *bot;
    pthread_mutex_t lock;

    outgoing_t outgoing[OUTGOING_MAX];
    size_t outgoing_count;

    request_t requests[REQUEST_MAX];
    size_t request_count;

    snapshot_chat_t chats[SNAPSHOT_CHATS];
    size_t chat_count;

    peer_t peers[PEER_MAX];
    size_t peer_count;

    bool online;
    bool stop;
    bool snapshot_dirty;
    unsigned sent;
    unsigned send_failures;
    char last_send[IRC_TEXT_MAX];
    char identity[EVERGRAM_IDENTITY_SIZE];
    char nickname[EVERGRAM_NICKNAME_SIZE];
};

/* --- helpers (call with the lock held unless stated) ---------------------- */

static evergram_t *client_of(bridge_t *bridge) {
    return evergram_bot_client(bridge->bot);
}

static bridge_t *bridge_from(evergram_t *eg) {
    evergram_bot_t *bot = evergram_bot_from_client(eg);
    return bot != NULL ? evergram_bot_user_data(bot) : NULL;
}

/* IRC nicks cannot carry every character an XRPL address can, and a whole
 * address is too long, so peers get a short stable name derived from it. */
void bridge_peer_nick(const char *identity, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (identity == NULL || identity[0] == '\0') {
        return;
    }

    /* The address part of "<chainFamily>:<address>", which is what a person
     * recognises. It is used as-is: XRPL addresses already start with 'r', and
     * adding one of our own is how this used to produce "rrNPvaf...". */
    const char *address = strchr(identity, ':');
    address = address != NULL ? address + 1 : identity;

    size_t used = 0;
    for (const char *cursor = address;
         *cursor != '\0' && used + 1u < out_size && used < NICK_BODY; cursor++) {
        if (isalnum((unsigned char)*cursor)) {
            out[used++] = *cursor;
        }
    }
    if (used == 0) {
        snprintf(out, out_size, "peer%u", (unsigned)(strlen(identity) % 1000u));
        return;
    }
    out[used] = '\0';
}

/*
 * Makes a peer's nick unique among the table, keeping room for a numeric
 * suffix. Called when a peer is added and again when its profile nickname
 * arrives, because two people can pick the same nickname.
 *
 * Locked: callers hold `lock`.
 */
static void resolve_nick_collisions_locked(bridge_t *bridge, peer_t *peer) {
    for (size_t i = 0; i < bridge->peer_count; i++) {
        if (&bridge->peers[i] == peer || strcmp(bridge->peers[i].nick, peer->nick) != 0) {
            continue;
        }
        char unique[IRC_NICK_MAX];
        snprintf(unique, sizeof(unique), "%.*s%u", IRC_NICK_MAX - 8, peer->nick,
                 (unsigned)(bridge->peer_count % 1000u));
        snprintf(peer->nick, sizeof(peer->nick), "%s", unique);
        return;
    }
}

/* Defined with the queues below; a peer being remembered queues a profile
 * lookup, so this needs to be visible here. */
static void queue_request_locked(bridge_t *bridge, const request_t *request);

/* Locked: callers hold `lock`. */
static peer_t *find_peer_locked(bridge_t *bridge, const char *identity) {
    for (size_t i = 0; i < bridge->peer_count; i++) {
        if (strcmp(bridge->peers[i].identity, identity) == 0) {
            return &bridge->peers[i];
        }
    }
    return NULL;
}

/* Locked. Remembers a peer, disambiguating a nick collision. */
static peer_t *remember_peer_locked(bridge_t *bridge, const char *identity) {
    if (identity == NULL || identity[0] == '\0') {
        return NULL;
    }

    peer_t *existing = find_peer_locked(bridge, identity);
    if (existing != NULL) {
        return existing;
    }
    if (bridge->peer_count == PEER_MAX) {
        return NULL; /* the table is a convenience, never a correctness need */
    }

    peer_t *peer = &bridge->peers[bridge->peer_count];
    memset(peer, 0, sizeof(*peer));
    snprintf(peer->identity, sizeof(peer->identity), "%s", identity);
    bridge_peer_nick(identity, peer->nick, sizeof(peer->nick));
    bridge->peer_count++;

    resolve_nick_collisions_locked(bridge, peer);

    /* One lookup per peer: the nickname is what a client should see instead of
     * an address, and it costs a round trip, so it is queued rather than done
     * here. */
    if (!peer->profile_requested) {
        peer->profile_requested = true;
        request_t request;
        memset(&request, 0, sizeof(request));
        request.kind = REQ_PROFILE;
        snprintf(request.a, sizeof(request.a), "%s", identity);
        queue_request_locked(bridge, &request);
    }
    return peer;
}

/* Locked. */
static peer_t *peer_by_nick_locked(bridge_t *bridge, const char *nick) {
    for (size_t i = 0; i < bridge->peer_count; i++) {
        if (strcasecmp(bridge->peers[i].nick, nick) == 0) {
            return &bridge->peers[i];
        }
    }
    return NULL;
}

/* Resolves a nick to a peer, taking the lock itself. */
static bool peer_is_known(bridge_t *bridge, const char *nick) {
    pthread_mutex_lock(&bridge->lock);
    bool known = peer_by_nick_locked(bridge, nick) != NULL;
    pthread_mutex_unlock(&bridge->lock);
    return known;
}

/* Locked: resolves a target nick or identity key to an identity. */
static bool resolve_target_locked(bridge_t *bridge, const char *target, char *identity_out,
                                  size_t out_size) {
    if (target == NULL || target[0] == '\0') {
        return false;
    }
    if (strchr(target, ':') != NULL) { /* already an identity key */
        snprintf(identity_out, out_size, "%s", target);
        remember_peer_locked(bridge, identity_out);
        return true;
    }
    peer_t *peer = peer_by_nick_locked(bridge, target);
    if (peer == NULL) {
        return false;
    }
    snprintf(identity_out, out_size, "%s", peer->identity);
    return true;
}

/* "#<chat id>", with the id made safe for an IRC channel name. */
static void channel_for_chat(const char *chat_id, char *out, size_t out_size) {
    char safe[IRC_CHANNEL_MAX];
    size_t used = 0;
    for (const char *cursor = chat_id; *cursor != '\0' && used + 1u < sizeof(safe); cursor++) {
        char c = *cursor;
        safe[used++] = (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') ? c : '_';
    }
    safe[used] = '\0';
    snprintf(out, out_size, "#%s", safe);
}

/* Locked. */
static const snapshot_chat_t *chat_for_channel_locked(const bridge_t *bridge,
                                                      const char *channel) {
    const char *wanted = irc_channel_strip(channel);
    for (size_t i = 0; i < bridge->chat_count; i++) {
        char candidate[IRC_CHANNEL_MAX];
        channel_for_chat(bridge->chats[i].chat_id, candidate, sizeof(candidate));
        if (strcasecmp(irc_channel_strip(candidate), wanted) == 0) {
            return &bridge->chats[i];
        }
    }
    return NULL;
}

/* Locked. The one-on-one chat with a peer, when one exists. */
static const snapshot_chat_t *direct_chat_with_locked(const bridge_t *bridge,
                                                      const char *identity) {
    for (size_t i = 0; i < bridge->chat_count; i++) {
        if (strcmp(bridge->chats[i].type, EVERGRAM_CHAT_TYPE_ONE_ON_ONE) != 0) {
            continue;
        }
        for (size_t p = 0; p < bridge->chats[i].participant_count; p++) {
            if (strcmp(bridge->chats[i].participants[p], identity) == 0) {
                return &bridge->chats[i];
            }
        }
    }
    return NULL;
}

static void chat_topic(const snapshot_chat_t *chat, char *out, size_t out_size) {
    const char *type = chat->type[0] != '\0' ? chat->type : "chat";
    if (chat->name[0] != '\0') {
        snprintf(out, out_size, "%s (%s, %zu participants)", chat->name, type,
                 chat->participant_count);
    } else {
        snprintf(out, out_size, "%s chat %s (%zu participants)", type, chat->chat_id,
                 chat->participant_count);
    }
}

/* --- queues --------------------------------------------------------------- */

/* Locked. */
static void queue_line_locked(bridge_t *bridge, const char *target, const char *format, ...) {
    if (bridge->outgoing_count == OUTGOING_MAX) {
        /* Dropping is the only honest option: the client is not reading, and
         * growing without bound would hurt the whole bridge. */
        return;
    }

    outgoing_t *out = &bridge->outgoing[bridge->outgoing_count];
    snprintf(out->target, sizeof(out->target), "%s", target != NULL ? target : "*");

    va_list args;
    va_start(args, format);
    vsnprintf(out->line, sizeof(out->line), format, args);
    va_end(args);
    bridge->outgoing_count++;
}

static void queue_line(bridge_t *bridge, const char *target, const char *format, ...) {
    if (bridge == NULL) {
        return;
    }
    va_list args;
    char line[IRC_LINE_MAX];

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    pthread_mutex_lock(&bridge->lock);
    queue_line_locked(bridge, target, "%s", line);
    pthread_mutex_unlock(&bridge->lock);
}

/* Locked. */
static void queue_request_locked(bridge_t *bridge, const request_t *request) {
    if (bridge->request_count == REQUEST_MAX) {
        queue_line_locked(bridge, request->origin,
                          ":%s NOTICE %s :*** The gateway queue is full; try again shortly.",
                          IRC_SERVER_NAME,
                          request->origin[0] != '\0' ? request->origin : "*");
        return;
    }
    bridge->requests[bridge->request_count++] = *request;
}

/* Fills a request with its kind, target nick, and up to two arguments. */
static request_t make_request(const char *origin, request_kind_t kind, const char *a, const char *b,
                              const char *code, const char *text, bool create_target) {
    request_t request;
    memset(&request, 0, sizeof(request));
    request.kind = kind;
    snprintf(request.origin, sizeof(request.origin), "%s", origin != NULL ? origin : "");
    snprintf(request.a, sizeof(request.a), "%s", a != NULL ? a : "");
    snprintf(request.b, sizeof(request.b), "%s", b != NULL ? b : "");
    snprintf(request.code, sizeof(request.code), "%s", code != NULL ? code : "");
    snprintf(request.text, sizeof(request.text), "%s", text != NULL ? text : "");
    request.create_target = create_target;
    return request;
}

static void enqueue_request(bridge_t *bridge, const request_t *request) {
    pthread_mutex_lock(&bridge->lock);
    queue_request_locked(bridge, request);
    pthread_mutex_unlock(&bridge->lock);
}

/* --- outgoing delivery (IRC thread) -------------------------------------- */

void bridge_flush(bridge_t *bridge) {
    if (bridge == NULL) {
        return;
    }

    outgoing_t batch[OUTGOING_MAX];
    size_t count = 0;

    /* The queue is copied under the lock and written outside it: a slow client
     * must not block the gateway thread. */
    pthread_mutex_lock(&bridge->lock);
    count = bridge->outgoing_count;
    if (count > 0) {
        memcpy(batch, bridge->outgoing, count * sizeof(batch[0]));
        bridge->outgoing_count = 0;
    }
    pthread_mutex_unlock(&bridge->lock);

    for (size_t i = 0; i < count; i++) {
        if (strcmp(batch[i].target, "*") == 0) {
            for (size_t c = 0; c < irc_server_registered_count(bridge->server); c++) {
                irc_send_raw(irc_server_registered_at(bridge->server, c), batch[i].line);
            }
            continue;
        }

        /* Addressed by nick, so a client that left simply misses the line. */
        bool delivered = false;
        for (size_t c = 0; c < irc_server_client_count(bridge->server); c++) {
            irc_client_t *client = irc_server_client_at(bridge->server, c);
            if (strcasecmp(irc_client_nick(client), batch[i].target) == 0) {
                irc_send_raw(client, batch[i].line);
                delivered = true;
                break;
            }
        }
        if (!delivered && irc_server_registered_count(bridge->server) > 0) {
            /* A direct answer to someone who is gone is dropped, but a line
             * meant for "the account" goes to whoever is here. */
            for (size_t c = 0; c < irc_server_registered_count(bridge->server); c++) {
                irc_send_raw(irc_server_registered_at(bridge->server, c), batch[i].line);
            }
        }
    }
}

/* --- snapshot (gateway thread writes, both read) -------------------------- */

typedef struct {
    bridge_t *bridge;
} snapshot_context_t;

static bool visit_snapshot_chat(const evergram_chat_info_t *chat, void *context) {
    snapshot_context_t *snapshot = context;
    bridge_t *bridge = snapshot->bridge;

    if (bridge->chat_count == SNAPSHOT_CHATS) {
        return false;
    }

    snapshot_chat_t *entry = &bridge->chats[bridge->chat_count];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->chat_id, sizeof(entry->chat_id), "%s", chat->chat_id);
    snprintf(entry->type, sizeof(entry->type), "%s", chat->type);
    snprintf(entry->name, sizeof(entry->name), "%s", chat->name);

    for (size_t i = 0; i < chat->participant_count &&
                       entry->participant_count < SNAPSHOT_PARTICIPANTS;
         i++) {
        if (chat->participants[i] == NULL) {
            continue;
        }
        snprintf(entry->participants[entry->participant_count],
                 sizeof(entry->participants[entry->participant_count]), "%s",
                 chat->participants[i]);
        entry->participant_count++;

        /* Every participant becomes addressable by nick. */
        if (strcmp(chat->participants[i], bridge->identity) != 0) {
            remember_peer_locked(bridge, chat->participants[i]);
        }
    }
    bridge->chat_count++;
    return true;
}

void bridge_refresh_snapshot(bridge_t *bridge) {
    if (bridge == NULL) {
        return;
    }

    pthread_mutex_lock(&bridge->lock);
    bridge->chat_count = 0;
    snapshot_context_t context = {.bridge = bridge};
    evergram_chat_list(client_of(bridge), visit_snapshot_chat, &context);
    bridge->snapshot_dirty = false;
    pthread_mutex_unlock(&bridge->lock);
}

/* --- gateway requests ----------------------------------------------------- */

typedef struct {
    const char *identity;
    const evergram_chat_info_t *found;
} direct_lookup_t;

static bool visit_direct_chat(const evergram_chat_info_t *chat, void *context) {
    direct_lookup_t *lookup = context;
    if (strcmp(chat->type, EVERGRAM_CHAT_TYPE_ONE_ON_ONE) != 0) {
        return true;
    }
    for (size_t i = 0; i < chat->participant_count; i++) {
        if (chat->participants[i] != NULL && strcmp(chat->participants[i], lookup->identity) == 0) {
            lookup->found = chat;
            return false;
        }
    }
    return true;
}

/* Defined with the events section; the profile helpers announce a rename. */
static void broadcast(bridge_t *bridge, const char *format, ...);

/*
 * Turns an Evergram nickname into something an IRC client accepts: letters,
 * digits, '-' and '_' only, starting with a letter or '_', and short enough to
 * leave room for a collision suffix. Anything that sanitizes to nothing is
 * rejected, and the peer keeps its address nick instead.
 */
void bridge_sanitize_nickname(const char *nickname, char *out, size_t out_size) {
    size_t used = 0;
    out[0] = '\0';
    if (nickname == NULL) {
        return;
    }

    for (const char *cursor = nickname;
         *cursor != '\0' && used + 1u < out_size && used < NICK_BODY; cursor++) {
        unsigned char c = (unsigned char)*cursor;
        if (isalnum(c) || c == '-' || c == '_') {
            out[used++] = (char)c;
        }
    }
    out[used] = '\0';
    if (used == 0) {
        return;
    }

    if (!isalpha((unsigned char)out[0]) && out[0] != '_') {
        /* A nick may not start with a digit or '-': shift and prefix. */
        if (used + 1u < out_size && used < NICK_BODY) {
            memmove(out + 1, out, used + 1u);
            out[0] = '_';
        }
    }
}

/*
 * Adopts a peer's profile nickname as its IRC nick and tells the clients, the
 * way a server announces a NICK change. Presentation only: the identity stays
 * the key everything is stored and addressed by internally.
 */
static void adopt_profile_nick(bridge_t *bridge, const char *identity, const char *nickname) {
    char old_nick[IRC_NICK_MAX];
    char new_nick[IRC_NICK_MAX];
    bool changed = false;

    pthread_mutex_lock(&bridge->lock);
    peer_t *peer = find_peer_locked(bridge, identity);
    if (peer != NULL && nickname[0] != '\0') {
        snprintf(old_nick, sizeof(old_nick), "%s", peer->nick);
        snprintf(peer->profile_nickname, sizeof(peer->profile_nickname), "%s", nickname);
        snprintf(peer->nick, sizeof(peer->nick), "%s", nickname);
        resolve_nick_collisions_locked(bridge, peer);
        snprintf(new_nick, sizeof(new_nick), "%s", peer->nick);
        changed = strcmp(old_nick, new_nick) != 0;
    }
    pthread_mutex_unlock(&bridge->lock);

    if (!changed) {
        return;
    }

    /* The proper IRC form, so a client updates its own state... */
    queue_line(bridge, "*", ":%s!%s@%s NICK :%s", old_nick, identity, IRC_SERVER_NAME, new_nick);
    /* ...and a notice, because telnet users have no client state to update. */
    broadcast(bridge, "*** %s is %s (%s)", old_nick, new_nick, identity);
}

/* The one-on-one chat with a peer, straight from the SDK. Gateway thread only. */
static const evergram_chat_info_t *find_direct_chat(evergram_t *eg, const char *identity) {
    direct_lookup_t lookup = {.identity = identity, .found = NULL};
    evergram_chat_list(eg, visit_direct_chat, &lookup);
    return lookup.found;
}

/* Runs one queued request. Gateway thread only, so blocking calls are fine. */
static void run_request(bridge_t *bridge, const request_t *request) {
    evergram_t *eg = client_of(bridge);
    evergram_status_t status = EVERGRAM_OK;
    bool refresh = false;
    char answer[IRC_TEXT_MAX];
    answer[0] = '\0';

    switch (request->kind) {
    case REQ_SEND: {
        char resolved[EVERGRAM_CHAT_ID_SIZE];
        snprintf(resolved, sizeof(resolved), "%s", request->b);
        const char *chat_id = resolved;

        /* A target given as an identity is resolved here, against the SDK, not
         * against the IRC thread's snapshot: the snapshot can be a moment
         * behind, and the cost of being wrong is a second conversation the peer
         * is not reading. */
        if (chat_id[0] == '\0' && request->a[0] != '\0') {
            const evergram_chat_info_t *existing = find_direct_chat(eg, request->a);
            if (existing != NULL) {
                snprintf(resolved, sizeof(resolved), "%s", existing->chat_id);
                chat_id = resolved;
            }
        }

        if (chat_id[0] == '\0' && request->a[0] != '\0') {
            pthread_mutex_lock(&bridge->lock);
            peer_t *peer = find_peer_locked(bridge, request->a);
            if (peer != NULL && peer->last_chat_id[0] != '\0') {
                snprintf(resolved, sizeof(resolved), "%s", peer->last_chat_id);
                chat_id = resolved;
            }
            pthread_mutex_unlock(&bridge->lock);
        }

        if (chat_id[0] == '\0' && request->create_target) {
            const char *participants[1] = {request->a};
            const evergram_chat_info_t *chat = NULL;
            status = evergram_chat_create(eg, EVERGRAM_CHAT_TYPE_ONE_ON_ONE, participants, 1,
                                          20000, &chat);
            if (status == EVERGRAM_OK && chat != NULL) {
                snprintf(resolved, sizeof(resolved), "%s", chat->chat_id);
                chat_id = resolved;
                refresh = true;
            }
        }

        if (status == EVERGRAM_OK && chat_id[0] == '\0') {
            status = EVERGRAM_ERR_STATE; /* nothing to send into */
        }
        if (status == EVERGRAM_OK) {
            status = evergram_send(eg, chat_id, request->text);
            fprintf(stderr, "[evergram-irc] send to chat %s (%zu chars): %s\n", chat_id,
                    strlen(request->text), evergram_status_str(status));
            pthread_mutex_lock(&bridge->lock);
            bridge->sent += status == EVERGRAM_OK ? 1u : 0u;
            bridge->send_failures += status == EVERGRAM_OK ? 0u : 1u;
            snprintf(bridge->last_send, sizeof(bridge->last_send), "%s -> %s: %s",
                     request->a[0] != '\0' ? request->a : chat_id,
                     evergram_status_str(status),
                     status != EVERGRAM_OK && evergram_last_error(eg) != NULL
                         ? evergram_last_error(eg)
                         : "ok");
            pthread_mutex_unlock(&bridge->lock);
        }
        break;
    }
    case REQ_LEAVE:
        status = evergram_chat_leave(eg, request->a, 20000);
        snprintf(answer, sizeof(answer), "*** Left %s.", request->a);
        refresh = true;
        break;
    case REQ_JOIN_CODE:
        status = evergram_chat_request_join(eg, request->code, 20000);
        snprintf(answer, sizeof(answer), "*** Asked to join with code %s.", request->code);
        refresh = true;
        break;
    case REQ_ADD:
        status = evergram_chat_add_participant(eg, request->b, request->a, 20000);
        snprintf(answer, sizeof(answer), "*** Added %s to %s.", request->a, request->b);
        refresh = true;
        break;
    case REQ_REMOVE:
        status = evergram_chat_remove_participant(eg, request->b, request->a, 20000);
        snprintf(answer, sizeof(answer), "*** Removed %s from %s.", request->a, request->b);
        refresh = true;
        break;
    case REQ_ACCEPT_CHAT: {
        const evergram_chat_info_t *chat = NULL;
        status = evergram_chat_request_accept(eg, request->a, 20000, &chat);
        if (status == EVERGRAM_OK && chat != NULL) {
            char channel[IRC_CHANNEL_MAX];
            channel_for_chat(chat->chat_id, channel, sizeof(channel));
            snprintf(answer, sizeof(answer), "*** Accepted %s; the conversation is %s.", request->a,
                     channel);
        }
        refresh = true;
        break;
    }
    case REQ_DECLINE_CHAT:
        status = evergram_chat_request_decline(eg, request->a, 20000);
        snprintf(answer, sizeof(answer), "*** Declined the chat request from %s.", request->a);
        break;
    case REQ_ACCEPT_INVITE:
        status = evergram_group_invite_accept(eg, request->a, 20000);
        snprintf(answer, sizeof(answer), "*** Accepted the invite to %s.", request->a);
        refresh = true;
        break;
    case REQ_DENY_JOIN:
        status = evergram_join_request_deny(eg, request->b, request->a, 20000);
        snprintf(answer, sizeof(answer), "*** Denied the join request from %s.", request->a);
        break;
    case REQ_REFRESH:
        evergram_sync_chats(eg);
        refresh = false; /* the response will mark the snapshot dirty */
        break;
    case REQ_SET_NICK: {
        /* A nick is at most IRC_NICK_MAX; the request buffer is a message
         * buffer, so it is truncated here rather than copied wholesale. */
        char nickname[IRC_NICK_MAX];
        snprintf(nickname, sizeof(nickname), "%.*s", IRC_NICK_MAX - 1, request->text);

        status = evergram_profile_set(eg, nickname, NULL, NULL, 20000, NULL);
        if (status == EVERGRAM_OK) {
            pthread_mutex_lock(&bridge->lock);
            snprintf(bridge->nickname, sizeof(bridge->nickname), "%s", nickname);
            pthread_mutex_unlock(&bridge->lock);
        }
        snprintf(answer, sizeof(answer), "*** Renamed to %s on Evergram.", nickname);
        break;
    }
    case REQ_PROFILE: {
        evergram_profile_t profile;
        memset(&profile, 0, sizeof(profile));
        evergram_status_t fetched = evergram_profile_get(eg, request->a, 20000, &profile);
        if (fetched == EVERGRAM_OK) {
            char nickname[EVERGRAM_NICKNAME_SIZE];
            bridge_sanitize_nickname(profile.nickname, nickname, sizeof(nickname));
            if (nickname[0] != '\0') {
                adopt_profile_nick(bridge, request->a, nickname);
            } else {
                fprintf(stderr, "[evergram-irc] %s has no usable nickname in its profile\n",
                        request->a);
            }
        } else {
            /* No profile (or a refusal) is not an error worth telling a client
             * about: the address nick stays, which is always valid. */
            fprintf(stderr, "[evergram-irc] no profile for %s: %s\n", request->a,
                    evergram_status_str(fetched));
        }
        return; /* nothing to report, and never a failure */
    }
    default:
        return;
    }

    if (request->origin[0] != '\0' && answer[0] != '\0') {
        if (status == EVERGRAM_OK) {
            queue_line(bridge, request->origin, ":%s NOTICE %s :%s", IRC_SERVER_NAME,
                       request->origin, answer);
        }
    }

    if (status != EVERGRAM_OK) {
        const char *detail = evergram_last_error(eg);
        queue_line(bridge, request->origin[0] != '\0' ? request->origin : "*",
                   ":%s NOTICE %s :*** The gateway refused: %s%s%s", IRC_SERVER_NAME,
                   request->origin[0] != '\0' ? request->origin : "*",
                   evergram_status_str(status), detail != NULL ? " - " : "",
                   detail != NULL ? detail : "");
    }

    if (refresh) {
        pthread_mutex_lock(&bridge->lock);
        bridge->snapshot_dirty = true;
        pthread_mutex_unlock(&bridge->lock);
    }
}

bool bridge_gateway_step(bridge_t *bridge) {
    if (bridge == NULL) {
        return false;
    }

    /* This is the call that can block for seconds while the transport retries;
     * it lives here so the IRC side never waits for it. */
    evergram_bot_poll(bridge->bot, 50);

    pthread_mutex_lock(&bridge->lock);
    bool online = bridge->online;
    bool dirty = bridge->snapshot_dirty;
    bool stop = bridge->stop;
    pthread_mutex_unlock(&bridge->lock);

    if (stop) {
        return false;
    }

    if (online) {
        request_t request;
        bool have_request = false;

        pthread_mutex_lock(&bridge->lock);
        if (bridge->request_count > 0) {
            request = bridge->requests[0];
            bridge->request_count--;
            for (size_t i = 0; i < bridge->request_count; i++) {
                bridge->requests[i] = bridge->requests[i + 1u];
            }
            have_request = true;
        }
        pthread_mutex_unlock(&bridge->lock);

        if (have_request) {
            run_request(bridge, &request);
            /* A request may have changed the chat list (created, joined, left),
             * so the snapshot is re-read right away. */
            dirty = true;
        }
    }

    if (dirty) {
        bridge_refresh_snapshot(bridge);
    }
    return true;
}

/* --- IRC commands --------------------------------------------------------- */

static void send_welcome(bridge_t *bridge, irc_client_t *client) {
    const char *nick = irc_client_nick(client);
    char identity[EVERGRAM_IDENTITY_SIZE];

    pthread_mutex_lock(&bridge->lock);
    snprintf(identity, sizeof(identity), "%s", bridge->identity);
    pthread_mutex_unlock(&bridge->lock);

    /* The network introduces itself as EVERGRAM, which is what a client shows
     * in its server window and what WHOIS reports. */
    irc_notice(client, "*** Welcome to %s - your Evergram account, bridged to IRC.",
               IRC_NETWORK_NAME);
    irc_notice(client, "*** Account: %s", identity[0] != '\0' ? identity : "(connecting...)");
    irc_notice(client, "*** Type HELP for the command map.");

    irc_send_numeric(client, 1, ":Welcome to the EVERGRAM IRC bridge");
    char rest[IRC_LINE_MAX];
    snprintf(rest, sizeof(rest), "Your host is %s, running version %s", IRC_SERVER_NAME,
             IRC_VERSION);
    irc_send_numeric(client, 2, rest);
    snprintf(rest, sizeof(rest), "This server was created for the EVERGRAM protocol bridge");
    irc_send_numeric(client, 3, rest);
    snprintf(rest, sizeof(rest), "%s %s iow nt", IRC_SERVER_NAME, IRC_VERSION);
    irc_send_numeric(client, 4, rest);
    snprintf(rest, sizeof(rest),
             "CHANTYPES=# PREFIX=(o)@ NICKLEN=%d CHANNELLEN=%d NETWORK=%s CASEMAPPING=ascii "
             ":are supported by this server",
             IRC_NICK_MAX - 1, IRC_CHANNEL_MAX - 1, IRC_NETWORK_NAME);
    irc_send_numeric(client, 5, rest);
    (void)nick;

    irc_send_numeric(client, 375, ":- EVERGRAM message of the day -");
    irc_send_numeric(client, 372,
                     ":- Every channel here is an Evergram chat, every nick an identity.");
    irc_send_numeric(client, 372,
                     ":- /LIST shows your chats, /WHOIS shows the identity behind a nick.");
    irc_send_numeric(client, 376, ":End of /MOTD command.");
}

static void send_names(bridge_t *bridge, irc_client_t *client, const char *channel,
                       const snapshot_chat_t *chat) {
    char names[IRC_TOPIC_MAX];
    names[0] = '\0';
    size_t used = 0;

    pthread_mutex_lock(&bridge->lock);
    for (size_t i = 0; chat != NULL && i < chat->participant_count; i++) {
        peer_t *peer = find_peer_locked(bridge, chat->participants[i]);
        const char *nick = peer != NULL ? peer->nick : chat->participants[i];
        int written = snprintf(names + used, sizeof(names) - used, "%s%s", used > 0 ? " " : "",
                               nick);
        if (written < 0 || (size_t)written >= sizeof(names) - used) {
            break;
        }
        used += (size_t)written;
    }
    pthread_mutex_unlock(&bridge->lock);

    if (used == 0) {
        snprintf(names, sizeof(names), "%s", irc_client_nick(client));
    }

    char rest[IRC_LINE_MAX];
    snprintf(rest, sizeof(rest), "= %s :%s", channel, names);
    irc_send_numeric(client, 353, rest);
    snprintf(rest, sizeof(rest), "%s :End of /NAMES list", channel);
    irc_send_numeric(client, 366, rest);
}

static void cmd_list(bridge_t *bridge, irc_client_t *client) {
    irc_send_numeric(client, 321, "Channel :Users Name");

    pthread_mutex_lock(&bridge->lock);
    size_t count = bridge->chat_count;
    for (size_t i = 0; i < count; i++) {
        char channel[IRC_CHANNEL_MAX];
        char topic[IRC_TOPIC_MAX];
        char rest[IRC_LINE_MAX];
        channel_for_chat(bridge->chats[i].chat_id, channel, sizeof(channel));
        chat_topic(&bridge->chats[i], topic, sizeof(topic));
        snprintf(rest, sizeof(rest), "%s %zu :%s", channel, bridge->chats[i].participant_count,
                 topic);
        irc_send_numeric(client, 322, rest);
    }
    pthread_mutex_unlock(&bridge->lock);

    irc_send_numeric(client, 323, ":End of /LIST");
    if (count == 0) {
        /* Either the account has no chats, or this view is behind. Asking is
         * cheap and turns the second case into a working /LIST. */
        request_t request = make_request("", REQ_REFRESH, NULL, NULL, NULL, NULL, false);
        enqueue_request(bridge, &request);
        irc_notice(client, "*** No chats in view. Asked Evergram to refresh; /LIST again in a "
                           "second, and /STATUS says how many are known.");
    }
}

static void cmd_join(bridge_t *bridge, irc_client_t *client, const char *target) {
    if (target == NULL || target[0] == '\0') {
        irc_send_numeric(client, 461, "JOIN :Not enough parameters");
        return;
    }

    /* A bare token that is not a channel is an invite code. */
    if (target[0] != '#') {
        enqueue_request(bridge, &(request_t){0});
        request_t request = make_request(irc_client_nick(client), REQ_JOIN_CODE, NULL, NULL, target,
                                         NULL, false);
        enqueue_request(bridge, &request);
        irc_notice(client, "*** Resolving invite code %s...", target);
        return;
    }

    char channel[IRC_CHANNEL_MAX];
    irc_channel_name(target, channel, sizeof(channel));

    pthread_mutex_lock(&bridge->lock);
    const snapshot_chat_t *chat = chat_for_channel_locked(bridge, channel);
    pthread_mutex_unlock(&bridge->lock);

    if (chat == NULL) {
        irc_send_numeric(client, 403, "JOIN :No such channel");
        irc_notice(client, "*** %s is not one of your chats. /LIST shows them.", channel);
        return;
    }

    if (!irc_client_add_channel(client, channel)) {
        irc_notice(client, "*** Too many channels open in this client.");
        return;
    }

    char prefix[EVERGRAM_NICKNAME_SIZE + EVERGRAM_IDENTITY_SIZE + 32];
    snprintf(prefix, sizeof(prefix), "%s!%s@%s", irc_client_nick(client), irc_client_user(client),
             irc_client_host(client));
    irc_send_from(client, prefix, "JOIN", channel, "");

    char topic[IRC_TOPIC_MAX];
    char rest[IRC_LINE_MAX];
    pthread_mutex_lock(&bridge->lock);
    chat_topic(chat, topic, sizeof(topic));
    pthread_mutex_unlock(&bridge->lock);
    snprintf(rest, sizeof(rest), "%s :%s", channel, topic);
    irc_send_numeric(client, 332, rest);
    send_names(bridge, client, channel, chat);
}

static void cmd_part(bridge_t *bridge, irc_client_t *client, const char *target) {
    char channel[IRC_CHANNEL_MAX];
    irc_channel_name(target, channel, sizeof(channel));
    if (!irc_client_in_channel(client, channel)) {
        irc_send_numeric(client, 442, "PART :You're not on that channel");
        return;
    }

    char prefix[EVERGRAM_NICKNAME_SIZE + EVERGRAM_IDENTITY_SIZE + 32];
    snprintf(prefix, sizeof(prefix), "%s!%s@%s", irc_client_nick(client), irc_client_user(client),
             irc_client_host(client));
    irc_send_from(client, prefix, "PART", channel, "");
    irc_client_remove_channel(client, channel);

    pthread_mutex_lock(&bridge->lock);
    const snapshot_chat_t *chat = chat_for_channel_locked(bridge, channel);
    bool is_group = chat != NULL && strcmp(chat->type, EVERGRAM_CHAT_TYPE_GROUP) == 0;
    char chat_id[EVERGRAM_CHAT_ID_SIZE];
    snprintf(chat_id, sizeof(chat_id), "%s", chat != NULL ? chat->chat_id : "");
    pthread_mutex_unlock(&bridge->lock);

    /* Leaving a group for real is a gateway call; a one-on-one just closes the
     * window, because Evergram has no notion of "close the conversation". */
    if (is_group) {
        request_t request = make_request(irc_client_nick(client), REQ_LEAVE, chat_id, NULL, NULL,
                                         NULL, false);
        enqueue_request(bridge, &request);
    }
}

static void cmd_privmsg(bridge_t *bridge, irc_client_t *client, const irc_message_t *message,
                        bool is_notice) {
    if (message->param_count == 0 || !message->has_trailing) {
        irc_send_numeric(client, 412, ":No text to send");
        return;
    }

    const char *target = message->params[0];
    const char *text = message->trailing;
    request_t request;

    if (target[0] == '#' || target[0] == '&') {
        pthread_mutex_lock(&bridge->lock);
        const snapshot_chat_t *chat = chat_for_channel_locked(bridge, target);
        char chat_id[EVERGRAM_CHAT_ID_SIZE];
        snprintf(chat_id, sizeof(chat_id), "%s", chat != NULL ? chat->chat_id : "");
        pthread_mutex_unlock(&bridge->lock);

        if (chat_id[0] == '\0') {
            irc_send_numeric(client, 403, "PRIVMSG :No such channel");
            return;
        }
        request = make_request(irc_client_nick(client), REQ_SEND, NULL, chat_id, NULL, text,
                               false);
    } else {
        char identity[EVERGRAM_IDENTITY_SIZE];
        pthread_mutex_lock(&bridge->lock);
        bool resolved = resolve_target_locked(bridge, target, identity, sizeof(identity));
        const snapshot_chat_t *direct =
            resolved ? direct_chat_with_locked(bridge, identity) : NULL;
        char chat_id[EVERGRAM_CHAT_ID_SIZE];
        snprintf(chat_id, sizeof(chat_id), "%s", direct != NULL ? direct->chat_id : "");
        pthread_mutex_unlock(&bridge->lock);

        if (!resolved) {
            irc_send_numeric(client, 401, "PRIVMSG :No such nick");
            return;
        }
        /* With no conversation yet the gateway thread creates it and sends, so
         * the message is not lost in the round trip. */
        request = make_request(irc_client_nick(client), REQ_SEND, identity, chat_id, NULL, text,
                               chat_id[0] == '\0');
    }

    enqueue_request(bridge, &request);

    /*
     * No echo. A real IRC server never sends a client its own PRIVMSG back -
     * the client shows what its user typed - and echoing here made the line
     * look like an incoming message (and appear twice in a graphical client).
     * A send that fails is reported by the gateway thread with a NOTICE, which
     * is where a delivery problem belongs.
     */
    (void)is_notice;
    (void)target;
}

static void cmd_names(bridge_t *bridge, irc_client_t *client, const char *target) {
    char channel[IRC_CHANNEL_MAX];
    irc_channel_name(target, channel, sizeof(channel));

    pthread_mutex_lock(&bridge->lock);
    const snapshot_chat_t *chat = chat_for_channel_locked(bridge, channel);
    pthread_mutex_unlock(&bridge->lock);

    if (chat == NULL) {
        irc_send_numeric(client, 403, "NAMES :No such channel");
        return;
    }
    send_names(bridge, client, channel, chat);
}

static void cmd_who(bridge_t *bridge, irc_client_t *client, const char *mask) {
    (void)mask;
    pthread_mutex_lock(&bridge->lock);
    size_t count = bridge->peer_count;
    for (size_t i = 0; i < count; i++) {
        char rest[IRC_LINE_MAX];
        snprintf(rest, sizeof(rest), "* %s %s %s %s :0 %s", bridge->peers[i].nick,
                 bridge->peers[i].identity, IRC_SERVER_NAME, bridge->peers[i].nick,
                 bridge->peers[i].identity);
        irc_send_numeric(client, 352, rest);
    }
    pthread_mutex_unlock(&bridge->lock);
    irc_send_numeric(client, 315, "* :End of /WHO list");
}

static void whois_one(bridge_t *bridge, irc_client_t *client, const char *nick) {
    char identity[EVERGRAM_IDENTITY_SIZE];
    char channels[IRC_TOPIC_MAX];
    channels[0] = '\0';

    bool is_self = false;
    for (size_t i = 0; i < irc_server_client_count(bridge->server); i++) {
        if (strcasecmp(irc_client_nick(irc_server_client_at(bridge->server, i)), nick) == 0) {
            is_self = true;
            break;
        }
    }

    pthread_mutex_lock(&bridge->lock);

    peer_t *peer = peer_by_nick_locked(bridge, nick);
    if (is_self) {
        /* Asking about yourself reports the Evergram account, which is what
         * the network line in the client shows. */
        snprintf(identity, sizeof(identity), "%s", bridge->identity);
    } else if (peer != NULL) {
        snprintf(identity, sizeof(identity), "%s", peer->identity);
    } else if (strchr(nick, ':') != NULL) {
        snprintf(identity, sizeof(identity), "%s", nick);
    } else {
        pthread_mutex_unlock(&bridge->lock);
        irc_send_numeric(client, 401, "WHOIS :No such nick");
        return;
    }

    size_t used = 0;
    for (size_t c = 0; c < bridge->chat_count; c++) {
        bool member = is_self; /* the account is in all of its own chats */
        for (size_t p = 0; !member && p < bridge->chats[c].participant_count; p++) {
            if (strcmp(bridge->chats[c].participants[p], identity) == 0) {
                member = true;
            }
        }
        if (!member) {
            continue;
        }
        char channel[IRC_CHANNEL_MAX];
        channel_for_chat(bridge->chats[c].chat_id, channel, sizeof(channel));
        int written = snprintf(channels + used, sizeof(channels) - used, "%s%s",
                               used > 0 ? " " : "", channel);
        if (written < 0 || (size_t)written >= sizeof(channels) - used) {
            break;
        }
        used += (size_t)written;
    }
    pthread_mutex_unlock(&bridge->lock);

    char rest[IRC_LINE_MAX];
    snprintf(rest, sizeof(rest), "%s %s %s * :%s", identity, IRC_SERVER_NAME, IRC_SERVER_NAME,
             IRC_NETWORK_NAME);
    irc_send_numeric(client, 311, rest);
    snprintf(rest, sizeof(rest), "%s %s :%s IRC bridge", identity, IRC_SERVER_NAME,
             IRC_NETWORK_NAME);
    irc_send_numeric(client, 312, rest);
    if (used > 0) {
        snprintf(rest, sizeof(rest), "%s :%s", identity, channels);
        irc_send_numeric(client, 319, rest);
    }
    snprintf(rest, sizeof(rest), "%s :%s - %s", identity, IRC_NETWORK_NAME,
             is_self ? "this is your Evergram account" : "Evergram identity, bridged to IRC");
    irc_send_numeric(client, 320, rest);
    snprintf(rest, sizeof(rest), "%s :End of /WHOIS list", identity);
    irc_send_numeric(client, 318, rest);
}

static void cmd_whois(bridge_t *bridge, irc_client_t *client, const irc_message_t *message) {
    if (message->param_count == 0) {
        irc_send_numeric(client, 431, ":No nick given");
        return;
    }
    char list[IRC_TARGET_MAX];
    snprintf(list, sizeof(list), "%s", message->params[0]);
    char *save = NULL;
    for (char *token = strtok_r(list, ",", &save); token != NULL;
         token = strtok_r(NULL, ",", &save)) {
        whois_one(bridge, client, token);
    }
}

/*
 * Answers the question this bridge will be asked most often: is it connected,
 * which account is it, and is anything being held back? A message that could
 * not be decrypted yet is held by the SDK's mailbox rather than dropped, so
 * that count is the first thing to look at when a message "does not arrive".
 */
static void cmd_status(bridge_t *bridge, irc_client_t *client) {
    pthread_mutex_lock(&bridge->lock);
    bool online = bridge->online;
    char identity[EVERGRAM_IDENTITY_SIZE];
    snprintf(identity, sizeof(identity), "%s", bridge->identity);
    size_t chats = bridge->chat_count;
    size_t peers = bridge->peer_count;
    size_t queued = bridge->request_count;
    pthread_mutex_unlock(&bridge->lock);

    evergram_bot_t *bot = bridge->bot;
    evergram_t *eg = evergram_bot_client(bot);

    irc_notice(client, "*** Evergram connection: %s", online ? "online" : "OFFLINE");
    irc_notice(client, "*** Account: %s (%s)", identity[0] != '\0' ? identity : "(unknown)",
               evergram_address(eg));
    irc_notice(client, "*** Chats known: %zu, identities seen: %zu", chats, peers);
    irc_notice(client, "*** Messages waiting for their chat key: %zu", evergram_bot_pending_count(bot));
    irc_notice(client, "*** Operations queued for the gateway: %zu", queued);
    irc_notice(client, "*** Reconnect attempts: %u", evergram_bot_reconnect_attempts(bot));

    pthread_mutex_lock(&bridge->lock);
    unsigned sent = bridge->sent;
    unsigned failures = bridge->send_failures;
    char last[IRC_TEXT_MAX];
    snprintf(last, sizeof(last), "%s", bridge->last_send);
    pthread_mutex_unlock(&bridge->lock);
    irc_notice(client, "*** Messages sent: %u (failed: %u)", sent, failures);
    if (last[0] != '\0') {
        irc_notice(client, "*** Last send: %s", last);
    }

    pthread_mutex_lock(&bridge->lock);
    for (size_t i = 0; i < bridge->peer_count; i++) {
        if (bridge->peers[i].last_chat_id[0] == '\0') {
            continue;
        }
        irc_notice(client, "*** reply to %s goes to chat %s", bridge->peers[i].nick,
                   bridge->peers[i].last_chat_id);
    }
    pthread_mutex_unlock(&bridge->lock);
    irc_notice(client, "*** A message only arrives here if it was sent to %s:",
               identity[0] != '\0' ? identity : "this account");
    irc_notice(client, "*** an existing chat of this account, or a new one from another "
                       "account, which /ACCEPT approves.");
}

static void cmd_users(bridge_t *bridge, irc_client_t *client) {
    pthread_mutex_lock(&bridge->lock);
    size_t count = bridge->peer_count;
    if (count == 0) {
        pthread_mutex_unlock(&bridge->lock);
        irc_notice(client, "*** No Evergram identities seen yet.");
        return;
    }
    irc_notice(client, "*** %zu Evergram identities:", count);
    for (size_t i = 0; i < count; i++) {
        irc_notice(client, "***   %-14s %s", bridge->peers[i].nick, bridge->peers[i].identity);
    }
    pthread_mutex_unlock(&bridge->lock);
}

static void cmd_motd(irc_client_t *client) {
    irc_send_numeric(client, 375, ":- EVERGRAM message of the day -");
    irc_send_numeric(client, 372,
                     ":- Every channel here is an Evergram chat, every nick an identity.");
    irc_send_numeric(client, 372, ":- /LIST shows your chats, /WHOIS the identity behind a nick.");
    irc_send_numeric(client, 376, ":End of /MOTD command.");
}

static void cmd_userhost(bridge_t *bridge, irc_client_t *client, const irc_message_t *message) {
    char reply[IRC_TARGET_MAX];
    reply[0] = '\0';
    size_t used = 0;

    for (size_t i = 0; i < message->param_count; i++) {
        bool known = false;
        for (size_t c = 0; c < irc_server_client_count(bridge->server); c++) {
            known = known || strcasecmp(irc_client_nick(irc_server_client_at(bridge->server, c)),
                                        message->params[i]) == 0;
        }
        known = known || peer_is_known(bridge, message->params[i]);
        if (!known) {
            continue;
        }
        int written = snprintf(reply + used, sizeof(reply) - used, "%s%s=+%s@%s",
                               used > 0 ? " " : "", message->params[i], message->params[i],
                               IRC_SERVER_NAME);
        if (written < 0 || (size_t)written >= sizeof(reply) - used) {
            break;
        }
        used += (size_t)written;
    }

    char rest[IRC_LINE_MAX];
    snprintf(rest, sizeof(rest), ":%s", reply);
    irc_send_numeric(client, 302, rest);
}

static void cmd_ison(bridge_t *bridge, irc_client_t *client, const irc_message_t *message) {
    char reply[IRC_TARGET_MAX];
    reply[0] = '\0';
    size_t used = 0;

    for (size_t i = 0; i < message->param_count; i++) {
        bool known = false;
        for (size_t c = 0; c < irc_server_client_count(bridge->server); c++) {
            known = known || strcasecmp(irc_client_nick(irc_server_client_at(bridge->server, c)),
                                        message->params[i]) == 0;
        }
        known = known || peer_is_known(bridge, message->params[i]);
        if (!known) {
            continue;
        }
        int written = snprintf(reply + used, sizeof(reply) - used, "%s%s", used > 0 ? " " : "",
                               message->params[i]);
        if (written < 0 || (size_t)written >= sizeof(reply) - used) {
            break;
        }
        used += (size_t)written;
    }

    char rest[IRC_LINE_MAX];
    snprintf(rest, sizeof(rest), ":%s", reply);
    irc_send_numeric(client, 303, rest);
}

static void cmd_help(irc_client_t *client) {
    irc_notice(client, "*** Standard IRC works here:");
    irc_notice(client, "***   /LIST                 your Evergram chats");
    irc_notice(client, "***   /JOIN #<chat>         open a chat");
    irc_notice(client, "***   /JOIN <invite code>   ask to join a group by code");
    irc_notice(client, "***   /MSG <nick> <text>    send a message");
    irc_notice(client, "***   /NAMES /WHO /WHOIS /TOPIC /PART /QUIT");
    irc_notice(client, "*** Evergram extras:");
    irc_notice(client, "***   /STATUS                 connection, account, held messages");
    irc_notice(client, "***   /USERS                   every identity seen");
    irc_notice(client, "***   /APPROVE <#chat> <nick>  approve a join request");
    irc_notice(client, "***   /DENY <#chat> <nick>     deny a join request");
    irc_notice(client, "***   /ACCEPT <nick>           accept a chat request");
    irc_notice(client, "***   /DECLINE <nick>          decline a chat request");
    irc_notice(client, "***   /GJOIN <#chat>           accept a group invite");
    irc_notice(client, "***   /INVITE <nick> <#chat>   add someone to a chat");
    irc_notice(client, "***   /REMOVE <nick> <#chat>   remove someone from a chat");
}

void bridge_handle_line(bridge_t *bridge, irc_client_t *client, const char *line) {
    if (bridge == NULL || client == NULL || line == NULL) {
        return;
    }

    irc_message_t message;
    if (!irc_parse(line, &message)) {
        return;
    }

    char command[32];
    snprintf(command, sizeof(command), "%s", message.command);
    irc_upper(command);

    if (strcmp(command, "QUIT") == 0) {
        irc_notice(client, "*** Closing the bridge connection.");
        irc_server_drop_client(bridge->server, client);
        return;
    }

    if (strcmp(command, "CAP") == 0) {
        /* Answering with an empty capability list is the standard way to say
         * "nothing on offer". Remaining silent makes clients such as HexChat
         * wait for an answer that never comes. */
        const char *subcommand = message.param_count > 0 ? message.params[0] : "";
        if (strcasecmp(subcommand, "LS") == 0) {
            char reply[IRC_LINE_MAX];
            snprintf(reply, sizeof(reply), ":%s CAP * LS :", IRC_SERVER_NAME);
            irc_send_raw(client, reply);
        } else if (strcasecmp(subcommand, "REQ") == 0) {
            char reply[IRC_LINE_MAX];
            snprintf(reply, sizeof(reply), ":%s CAP * NAK :%.*s", IRC_SERVER_NAME,
                     IRC_TARGET_MAX, message.param_count > 1 ? message.params[1] : "");
            irc_send_raw(client, reply);
        }
        /* CAP END needs no answer. */
        return;
    }

    if (strcmp(command, "PING") == 0) {
        /* A client sends the token as a trailing parameter; echoing it back is
         * what keeps its liveness check honest. */
        const char *token = message.has_trailing
                                ? message.trailing
                                : (message.param_count > 0 ? message.params[0] : IRC_SERVER_NAME);
        /* A token is a short cookie, and the line has a hard limit anyway, so
         * only its first characters are echoed. */
        char reply[IRC_LINE_MAX];
        snprintf(reply, sizeof(reply), ":%s PONG %s :%.*s", IRC_SERVER_NAME, IRC_SERVER_NAME,
                 IRC_NICK_MAX, token);
        irc_send_raw(client, reply);
        return;
    }
    if (strcmp(command, "PONG") == 0) {
        return;
    }

    if (strcmp(command, "NICK") == 0) {
        if (message.param_count == 0) {
            irc_send_numeric(client, 431, ":No nickname given");
            return;
        }

        /* Before registration this is just the local nick. Afterwards it is a
         * rename request for the Evergram profile, which is the only name other
         * people ever see; the local nick follows it. */
        char previous[IRC_NICK_MAX];
        snprintf(previous, sizeof(previous), "%s", irc_client_nick(client));
        irc_client_set_nick(client, message.params[0]);

        if (irc_client_registered(client) && strcmp(previous, message.params[0]) != 0) {
            /* The profile is what other people see, and the nickname field is
             * the same one /WHOIS reports, so the two are kept in step. */
            request_t request = make_request(previous, REQ_SET_NICK, NULL, NULL, NULL,
                                             message.params[0], false);
            enqueue_request(bridge, &request);
            irc_notice(client, "*** Renaming your Evergram profile to %s...", message.params[0]);
        }
    } else if (strcmp(command, "USER") == 0) {
        if (irc_client_registered(client)) {
            irc_send_numeric(client, 462, ":You may not reregister");
            return;
        }
        irc_client_set_user(client, message.param_count > 0 ? message.params[0] : "user",
                            message.has_trailing ? message.trailing : NULL);
    }

    if (!irc_client_registered(client)) {
        if (irc_client_nick(client)[0] == '\0' || irc_client_user(client)[0] == '\0') {
            /* Wait quietly for both, as a real server does. Losing patience here
             * would greet every telnet user with a complaint about a command
             * they have not had the chance to send yet. */
            return;
        }
        irc_client_mark_registered(client);
        send_welcome(bridge, client);
    }

    if (strcmp(command, "NICK") == 0 || strcmp(command, "USER") == 0) {
        return;
    }

    if (strcmp(command, "LIST") == 0) {
        cmd_list(bridge, client);
    } else if (strcmp(command, "JOIN") == 0) {
        if (message.param_count == 0) {
            irc_send_numeric(client, 461, "JOIN :Not enough parameters");
            return;
        }
        char list[IRC_TARGET_MAX];
        snprintf(list, sizeof(list), "%s", message.params[0]);
        char *save = NULL;
        for (char *token = strtok_r(list, ",", &save); token != NULL;
             token = strtok_r(NULL, ",", &save)) {
            cmd_join(bridge, client, token);
        }
    } else if (strcmp(command, "PART") == 0) {
        if (message.param_count == 0) {
            irc_send_numeric(client, 461, "PART :Not enough parameters");
            return;
        }
        cmd_part(bridge, client, message.params[0]);
    } else if (strcmp(command, "PRIVMSG") == 0) {
        cmd_privmsg(bridge, client, &message, false);
    } else if (strcmp(command, "NOTICE") == 0) {
        cmd_privmsg(bridge, client, &message, true);
    } else if (strcmp(command, "NAMES") == 0) {
        if (message.param_count == 0) {
            irc_send_numeric(client, 366, "* :End of /NAMES list");
            return;
        }
        cmd_names(bridge, client, message.params[0]);
    } else if (strcmp(command, "WHO") == 0) {
        cmd_who(bridge, client, message.param_count > 0 ? message.params[0] : "*");
    } else if (strcmp(command, "WHOIS") == 0) {
        cmd_whois(bridge, client, &message);
    } else if (strcmp(command, "USERS") == 0) {
        cmd_users(bridge, client);
    } else if (strcmp(command, "STATUS") == 0) {
        cmd_status(bridge, client);
    } else if (strcmp(command, "HELP") == 0) {
        cmd_help(client);
    } else if (strcmp(command, "MOTD") == 0) {
        cmd_motd(client);
    } else if (strcmp(command, "USERHOST") == 0) {
        cmd_userhost(bridge, client, &message);
    } else if (strcmp(command, "ISON") == 0) {
        cmd_ison(bridge, client, &message);
    } else if (strcmp(command, "VERSION") == 0) {
        char rest[IRC_LINE_MAX];
        snprintf(rest, sizeof(rest), "%s %s :%s - Evergram bridged to IRC", IRC_VERSION,
                 IRC_SERVER_NAME, IRC_NETWORK_NAME);
        irc_send_numeric(client, 351, rest);
    } else if (strcmp(command, "LUSERS") == 0) {
        char rest[IRC_LINE_MAX];
        size_t clients = irc_server_registered_count(bridge->server);
        pthread_mutex_lock(&bridge->lock);
        size_t peers = bridge->peer_count;
        pthread_mutex_unlock(&bridge->lock);
        snprintf(rest, sizeof(rest), ":There are %zu clients and %zu Evergram identities", clients,
                 peers);
        irc_send_numeric(client, 251, rest);
        snprintf(rest, sizeof(rest), ":End of /LUSERS");
        irc_send_numeric(client, 255, rest);
    } else if (strcmp(command, "MODE") == 0) {
        if (message.param_count > 0 && message.params[0][0] == '#') {
            char rest[IRC_TARGET_MAX + 16];
            snprintf(rest, sizeof(rest), "%s +nt", message.params[0]);
            irc_send_numeric(client, 324, rest);
        } else {
            char rest[IRC_NICK_MAX + 8];
            snprintf(rest, sizeof(rest), "%s +i", irc_client_nick(client));
            irc_send_numeric(client, 221, rest);
        }
    } else if (strcmp(command, "TOPIC") == 0) {
        if (message.param_count == 0) {
            irc_send_numeric(client, 461, "TOPIC :Not enough parameters");
            return;
        }
        char channel[IRC_CHANNEL_MAX];
        irc_channel_name(message.params[0], channel, sizeof(channel));

        pthread_mutex_lock(&bridge->lock);
        const snapshot_chat_t *chat = chat_for_channel_locked(bridge, channel);
        char topic[IRC_TOPIC_MAX];
        if (chat != NULL) {
            chat_topic(chat, topic, sizeof(topic));
        }
        pthread_mutex_unlock(&bridge->lock);

        if (chat == NULL) {
            irc_send_numeric(client, 403, "TOPIC :No such channel");
            return;
        }
        char rest[IRC_LINE_MAX];
        snprintf(rest, sizeof(rest), "%s :%s", channel, topic);
        irc_send_numeric(client, 332, rest);
        snprintf(rest, sizeof(rest), "%s %s 0", channel, IRC_SERVER_NAME);
        irc_send_numeric(client, 333, rest);
        if (message.has_trailing) {
            irc_notice(client, "*** Topics are Evergram chat metadata and cannot be set here.");
        }
    } else if (strcmp(command, "AWAY") == 0) {
        irc_send_numeric(client, 305, ":You are no longer marked as being away");
    } else if (strcmp(command, "APPROVE") == 0 || strcmp(command, "DENY") == 0) {
        char identity[EVERGRAM_IDENTITY_SIZE];
        char chat_id[EVERGRAM_CHAT_ID_SIZE];
        bool ok = message.param_count >= 2;
        if (ok) {
            pthread_mutex_lock(&bridge->lock);
            ok = resolve_target_locked(bridge, message.params[1], identity, sizeof(identity));
            const snapshot_chat_t *chat = chat_for_channel_locked(bridge, message.params[0]);
            snprintf(chat_id, sizeof(chat_id), "%s", chat != NULL ? chat->chat_id : "");
            pthread_mutex_unlock(&bridge->lock);
            ok = ok && chat_id[0] != '\0';
        }
        if (!ok) {
            irc_notice(client, "*** Usage: /%s <#chat> <nick>", command);
            return;
        }
        request_t request =
            make_request(irc_client_nick(client),
                         strcmp(command, "APPROVE") == 0 ? REQ_ADD : REQ_DENY_JOIN, identity,
                         chat_id, NULL, NULL, false);
        enqueue_request(bridge, &request);
        irc_notice(client, "*** %s %s for %s...",
                   strcmp(command, "APPROVE") == 0 ? "Approving" : "Denying", message.params[1],
                   message.params[0]);
    } else if (strcmp(command, "ACCEPT") == 0 || strcmp(command, "DECLINE") == 0) {
        char identity[EVERGRAM_IDENTITY_SIZE];
        bool ok = message.param_count > 0;
        if (ok) {
            pthread_mutex_lock(&bridge->lock);
            ok = resolve_target_locked(bridge, message.params[0], identity, sizeof(identity));
            pthread_mutex_unlock(&bridge->lock);
        }
        if (!ok) {
            irc_send_numeric(client, 401, "ACCEPT :No such nick");
            return;
        }
        request_t request =
            make_request(irc_client_nick(client),
                         strcmp(command, "ACCEPT") == 0 ? REQ_ACCEPT_CHAT : REQ_DECLINE_CHAT,
                         identity, NULL, NULL, NULL, false);
        enqueue_request(bridge, &request);
        irc_notice(client, "*** %s %s...",
                   strcmp(command, "ACCEPT") == 0 ? "Accepting" : "Declining", message.params[0]);
    } else if (strcmp(command, "GJOIN") == 0) {
        if (message.param_count == 0) {
            irc_send_numeric(client, 461, "GJOIN :Usage: /GJOIN <#chat>");
            return;
        }
        char channel[IRC_CHANNEL_MAX];
        irc_channel_name(message.params[0], channel, sizeof(channel));

        pthread_mutex_lock(&bridge->lock);
        const snapshot_chat_t *chat = chat_for_channel_locked(bridge, channel);
        char chat_id[EVERGRAM_CHAT_ID_SIZE];
        snprintf(chat_id, sizeof(chat_id), "%s",
                 chat != NULL ? chat->chat_id : irc_channel_strip(message.params[0]));
        pthread_mutex_unlock(&bridge->lock);

        request_t request =
            make_request(irc_client_nick(client), REQ_ACCEPT_INVITE, chat_id, NULL, NULL, NULL,
                         false);
        enqueue_request(bridge, &request);
        irc_notice(client, "*** Accepting the invite to %s...", message.params[0]);
    } else if (strcmp(command, "INVITE") == 0) {
        char identity[EVERGRAM_IDENTITY_SIZE];
        char chat_id[EVERGRAM_CHAT_ID_SIZE];
        bool ok = message.param_count >= 2;
        if (ok) {
            pthread_mutex_lock(&bridge->lock);
            ok = resolve_target_locked(bridge, message.params[0], identity, sizeof(identity));
            const snapshot_chat_t *chat = chat_for_channel_locked(bridge, message.params[1]);
            snprintf(chat_id, sizeof(chat_id), "%s", chat != NULL ? chat->chat_id : "");
            pthread_mutex_unlock(&bridge->lock);
            ok = ok && chat_id[0] != '\0';
        }
        if (!ok) {
            irc_notice(client, "*** Usage: /INVITE <nick> <#chat>");
            return;
        }
        request_t request = make_request(irc_client_nick(client), REQ_ADD, identity, chat_id, NULL,
                                         NULL, false);
        enqueue_request(bridge, &request);
        irc_notice(client, "*** Inviting %s to %s...", message.params[0], message.params[1]);
    } else if (strcmp(command, "REMOVE") == 0 || strcmp(command, "KICK") == 0) {
        const char *nick = NULL;
        const char *channel = NULL;
        if (strcmp(command, "REMOVE") == 0 && message.param_count >= 2) {
            nick = message.params[0];
            channel = message.params[1];
        } else if (message.param_count >= 2) { /* KICK <#chat> <nick> */
            channel = message.params[0];
            nick = message.params[1];
        }
        char identity[EVERGRAM_IDENTITY_SIZE];
        char chat_id[EVERGRAM_CHAT_ID_SIZE];
        bool ok = nick != NULL && channel != NULL;
        if (ok) {
            pthread_mutex_lock(&bridge->lock);
            ok = resolve_target_locked(bridge, nick, identity, sizeof(identity));
            const snapshot_chat_t *chat = chat_for_channel_locked(bridge, channel);
            snprintf(chat_id, sizeof(chat_id), "%s", chat != NULL ? chat->chat_id : "");
            pthread_mutex_unlock(&bridge->lock);
            ok = ok && chat_id[0] != '\0';
        }
        if (!ok) {
            irc_notice(client, "*** Usage: /REMOVE <nick> <#chat>");
            return;
        }
        request_t request = make_request(irc_client_nick(client), REQ_REMOVE, identity, chat_id,
                                         NULL, NULL, false);
        enqueue_request(bridge, &request);
        irc_notice(client, "*** Removing %s from %s...", nick, channel);
    } else {
        char rest[IRC_TARGET_MAX];
        snprintf(rest, sizeof(rest), "%s :Unknown command", command);
        irc_send_numeric(client, 421, rest);
        irc_notice(client, "*** %s is not a command this bridge knows. Type HELP.", command);
    }
}

/* --- Evergram events (gateway thread) ------------------------------------ */

static void broadcast(bridge_t *bridge, const char *format, ...) {
    char text[IRC_TEXT_MAX];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    queue_line(bridge, "*", ":%s NOTICE * :%s", IRC_SERVER_NAME, text);
}

/* Our own messages are echoed locally by whoever sent them, so the gateway's
 * copy of them would only duplicate. */
/* The address part of "<chainFamily>:<address>"; identity keys in the protocol
 * are always in that form, but comparing only the address as well costs nothing
 * and removes a whole class of "why is my own message coming back" surprises. */
static const char *identity_address(const char *identity) {
    const char *colon = strchr(identity, ':');
    return colon != NULL ? colon + 1 : identity;
}

static bool is_own(bridge_t *bridge, const char *sender) {
    bool own = false;
    pthread_mutex_lock(&bridge->lock);
    own = strcmp(sender, bridge->identity) == 0 ||
          strcmp(identity_address(sender), identity_address(bridge->identity)) == 0;
    pthread_mutex_unlock(&bridge->lock);
    return own;
}

static void deliver_message(evergram_t *eg, bridge_t *bridge, const char *chat_id,
                            const char *sender, const char *text) {
    char target[IRC_TARGET_MAX];
    char own_nick[IRC_NICK_MAX] = "";
    bool direct = false;

    /* This runs on the gateway thread, so the SDK can be asked directly: the
     * snapshot may not have caught up with a chat created a moment ago, and
     * guessing wrong here would send a direct message to a channel nobody
     * joined. */
    const evergram_chat_info_t *chat = evergram_chat_get(eg, chat_id);
    if (chat != NULL) {
        direct = strcmp(chat->type, EVERGRAM_CHAT_TYPE_ONE_ON_ONE) == 0;
    } else {
        pthread_mutex_lock(&bridge->lock);
        for (size_t i = 0; i < bridge->chat_count; i++) {
            if (strcmp(bridge->chats[i].chat_id, chat_id) == 0) {
                direct = strcmp(bridge->chats[i].type, EVERGRAM_CHAT_TYPE_ONE_ON_ONE) == 0;
                break;
            }
        }
        pthread_mutex_unlock(&bridge->lock);
    }

    pthread_mutex_lock(&bridge->lock);
    peer_t *peer = find_peer_locked(bridge, sender);
    char peer_nick[IRC_NICK_MAX];
    snprintf(peer_nick, sizeof(peer_nick), "%s", peer != NULL ? peer->nick : "peer");
    pthread_mutex_unlock(&bridge->lock);

    if (direct) {
        /* A one-on-one is delivered to the client's own nick, as a DM. Every
         * connected client is the same account, so it goes to all of them. */
        snprintf(target, sizeof(target), "*");
        snprintf(own_nick, sizeof(own_nick), "%s", peer_nick);
    } else {
        channel_for_chat(chat_id, target, sizeof(target));
        snprintf(own_nick, sizeof(own_nick), "%s", peer_nick);
    }

    char prefix[EVERGRAM_NICKNAME_SIZE + EVERGRAM_IDENTITY_SIZE + 64];
    snprintf(prefix, sizeof(prefix), "%s!%s@%s", own_nick, sender, IRC_SERVER_NAME);

    /* A DM has to be addressed to a real nick, so it is queued per client. */
    if (direct) {
        pthread_mutex_lock(&bridge->lock);
        for (size_t i = 0; i < irc_server_client_count(bridge->server); i++) {
            irc_client_t *client = irc_server_client_at(bridge->server, i);
            const char *nick = irc_client_nick(client);
            if (nick[0] != '\0') {
                queue_line_locked(bridge, nick, ":%s PRIVMSG %s :%s", prefix, nick,
                                  text != NULL ? text : "");
            }
        }
        pthread_mutex_unlock(&bridge->lock);
        return;
    }

    queue_line(bridge, target, ":%s PRIVMSG %s :%s", prefix, target, text != NULL ? text : "");
}

static void on_message(evergram_t *eg, const evergram_message_t *message) {
    bridge_t *bridge = bridge_from(eg);
    if (bridge == NULL || message == NULL) {
        return;
    }
    if (is_own(bridge, message->sender)) {
        /* The gateway relays a sender's own message to its other devices; this
         * bridge is the client that sent it, so it drops its own copy. */
        fprintf(stderr, "[evergram-irc] dropped own message echoed back in %s\n",
                message->chat_id);
        return;
    }

    pthread_mutex_lock(&bridge->lock);
    peer_t *peer = remember_peer_locked(bridge, message->sender);
    if (peer != NULL) {
        snprintf(peer->last_chat_id, sizeof(peer->last_chat_id), "%s", message->chat_id);
    }
    pthread_mutex_unlock(&bridge->lock);

    fprintf(stderr, "[evergram-irc] message in %s from %s (%zu chars)\n",
            message->chat_id, message->sender,
            message->text != NULL ? strlen(message->text) : 0);

    /* A message can be the first sign of a chat the IRC side has not seen yet,
     * and the reply has to go back into this same conversation. */
    pthread_mutex_lock(&bridge->lock);
    bridge->snapshot_dirty = true;
    pthread_mutex_unlock(&bridge->lock);

    deliver_message(eg, bridge, message->chat_id, message->sender, message->text);
}

static const char *peer_nick_for(bridge_t *bridge, const char *identity, char *out, size_t size) {
    pthread_mutex_lock(&bridge->lock);
    peer_t *peer = remember_peer_locked(bridge, identity);
    snprintf(out, size, "%s", peer != NULL ? peer->nick : "peer");
    pthread_mutex_unlock(&bridge->lock);
    return out;
}

static void on_message_edited(evergram_t *eg, const evergram_message_edited_t *edited) {
    bridge_t *bridge = bridge_from(eg);
    if (bridge == NULL || edited == NULL) {
        return;
    }
    char nick[IRC_NICK_MAX];
    broadcast(bridge, "* %s edited a message: %s", peer_nick_for(bridge, edited->sender, nick,
                                                                 sizeof(nick)),
              edited->text != NULL ? edited->text : "(unreadable)");
}

static void on_message_deleted(evergram_t *eg, const evergram_message_deleted_t *deleted) {
    bridge_t *bridge = bridge_from(eg);
    if (bridge == NULL || deleted == NULL) {
        return;
    }
    char nick[IRC_NICK_MAX];
    broadcast(bridge, "* %s deleted a message",
              peer_nick_for(bridge, deleted->sender, nick, sizeof(nick)));
}

static void on_reaction(evergram_t *eg, const evergram_reaction_t *reaction) {
    bridge_t *bridge = bridge_from(eg);
    if (bridge == NULL || reaction == NULL) {
        return;
    }
    char nick[IRC_NICK_MAX];
    broadcast(bridge, "* %s %s a message",
              peer_nick_for(bridge, reaction->sender, nick, sizeof(nick)),
              reaction->removed ? "removed their reaction to" : reaction->emoji);
}

static void on_typing(evergram_t *eg, const evergram_typing_event_t *event) {
    /* IRC has nowhere to show this, and the signals are constant. */
    (void)eg;
    (void)event;
}

static void on_join_request(evergram_t *eg, const evergram_join_request_t *request) {
    bridge_t *bridge = bridge_from(eg);
    if (bridge == NULL || request == NULL) {
        return;
    }
    fprintf(stderr, "[evergram-irc] join request in %s from %s\n", request->chat_id,
            request->identity);
    char nick[IRC_NICK_MAX];
    peer_nick_for(bridge, request->identity, nick, sizeof(nick));
    char channel[IRC_CHANNEL_MAX];
    channel_for_chat(request->chat_id, channel, sizeof(channel));
    broadcast(bridge, "*** %s (%s) asked to join %s%s%s - /APPROVE %s %s or /DENY %s %s", nick,
              request->identity, channel, request->name[0] != '\0' ? " " : "", request->name,
              channel, nick, channel, nick);
}

static void on_chat_request(evergram_t *eg, const evergram_chat_request_t *request) {
    bridge_t *bridge = bridge_from(eg);
    if (bridge == NULL || request == NULL) {
        return;
    }
    fprintf(stderr, "[evergram-irc] chat request from %s\n", request->from_identity);
    char nick[IRC_NICK_MAX];
    peer_nick_for(bridge, request->from_identity, nick, sizeof(nick));
    broadcast(bridge, "*** %s (%s) wants to open a conversation - /ACCEPT %s or /DECLINE %s", nick,
              request->from_identity, nick, nick);
}

static void on_group_invite(evergram_t *eg, const evergram_group_invite_t *invite) {
    bridge_t *bridge = bridge_from(eg);
    if (bridge == NULL || invite == NULL) {
        return;
    }
    fprintf(stderr, "[evergram-irc] group invite to %s from %s\n", invite->chat_id,
            invite->invited_by);
    char nick[IRC_NICK_MAX];
    peer_nick_for(bridge, invite->invited_by, nick, sizeof(nick));
    char channel[IRC_CHANNEL_MAX];
    channel_for_chat(invite->chat_id, channel, sizeof(channel));
    broadcast(bridge, "*** Invited to the group \"%s\" (%s) by %s - /GJOIN %s",
              invite->name[0] != '\0' ? invite->name : "(unnamed)", channel, nick, channel);
}

static void on_chat_removed(evergram_t *eg, const char *chat_id) {
    bridge_t *bridge = bridge_from(eg);
    if (bridge == NULL || chat_id == NULL) {
        return;
    }
    char channel[IRC_CHANNEL_MAX];
    channel_for_chat(chat_id, channel, sizeof(channel));
    broadcast(bridge, "*** %s is gone (left, deleted, or you were removed).", channel);

    pthread_mutex_lock(&bridge->lock);
    bridge->snapshot_dirty = true;
    pthread_mutex_unlock(&bridge->lock);
}

static void on_error(evergram_t *eg, evergram_status_t status, const char *detail) {
    bridge_t *bridge = bridge_from(eg);
    if (bridge == NULL) {
        return;
    }
    broadcast(bridge, "*** Evergram error: %s%s%s", evergram_status_str(status),
              detail != NULL ? " - " : "", detail != NULL ? detail : "");
}

static void on_connected(evergram_t *eg) {
    bridge_t *bridge = bridge_from(eg);
    if (bridge == NULL) {
        return;
    }
    pthread_mutex_lock(&bridge->lock);
    bridge->online = true;
    snprintf(bridge->identity, sizeof(bridge->identity), "%s", evergram_identity_key(eg));
    bridge->snapshot_dirty = true;
    pthread_mutex_unlock(&bridge->lock);

    fprintf(stderr, "[evergram-irc] connected to Evergram as %s\n", evergram_identity_key(eg));
    broadcast(bridge, "*** Connected to Evergram as %s — %zu chat(s) known so far.",
              evergram_identity_key(eg), evergram_chat_count(eg));
}

static void on_disconnected(evergram_t *eg) {
    bridge_t *bridge = bridge_from(eg);
    if (bridge == NULL) {
        return;
    }
    pthread_mutex_lock(&bridge->lock);
    bridge->online = false;
    pthread_mutex_unlock(&bridge->lock);
    fprintf(stderr, "[evergram-irc] Evergram connection lost\n");
    broadcast(bridge, "*** Evergram connection lost; reconnecting.");
}

static void on_presence(evergram_t *eg, const evergram_presence_t *presence) {
    bridge_t *bridge = bridge_from(eg);
    if (bridge == NULL || presence == NULL) {
        return;
    }
    char nick[IRC_NICK_MAX];
    broadcast(bridge, "* %s is now %s",
              peer_nick_for(bridge, presence->identity_key, nick, sizeof(nick)),
              presence->online ? "online" : "offline");
}

static void on_profile_updated(evergram_t *eg, const evergram_profile_t *profile) {
    bridge_t *bridge = bridge_from(eg);
    if (bridge == NULL || profile == NULL) {
        return;
    }

    /* A rename in Evergram should rename the nick here too. */
    if (profile->nickname[0] != '\0') {
        char nickname[EVERGRAM_NICKNAME_SIZE];
        bridge_sanitize_nickname(profile->nickname, nickname, sizeof(nickname));
        if (nickname[0] != '\0' && !is_own(bridge, profile->identity_key)) {
            adopt_profile_nick(bridge, profile->identity_key, nickname);
            return;
        }
    }

    broadcast(bridge, "* profile updated: %s%s%s",
              profile->nickname[0] != '\0' ? profile->nickname : "(no nickname)",
              profile->bio[0] != '\0' ? " - " : "", profile->bio);
}

/* --- lifecycle ------------------------------------------------------------ */

bridge_t *bridge_create(irc_server_t *server, const char *nickname) {
    if (server == NULL) {
        return NULL;
    }

    bridge_t *bridge = calloc(1, sizeof(*bridge));
    if (bridge == NULL) {
        return NULL;
    }
    bridge->server = server;
    if (nickname != NULL) {
        snprintf(bridge->nickname, sizeof(bridge->nickname), "%s", nickname);
    }
    if (pthread_mutex_init(&bridge->lock, NULL) != 0) {
        free(bridge);
        return NULL;
    }
    return bridge;
}

bool bridge_attach(bridge_t *bridge, evergram_bot_t *bot) {
    if (bridge == NULL || bot == NULL || bridge->bot != NULL) {
        return false;
    }
    bridge->bot = bot;

    evergram_t *eg = evergram_bot_client(bot);
    pthread_mutex_lock(&bridge->lock);
    snprintf(bridge->identity, sizeof(bridge->identity), "%s", evergram_identity_key(eg));
    pthread_mutex_unlock(&bridge->lock);

    evergram_bot_on_message(bot, on_message);
    evergram_bot_on_message_edited(bot, on_message_edited);
    evergram_bot_on_message_deleted(bot, on_message_deleted);
    evergram_bot_on_reaction(bot, on_reaction);
    evergram_bot_on_typing(bot, on_typing);
    evergram_bot_on_join_request(bot, on_join_request);
    evergram_bot_on_chat_request(bot, on_chat_request);
    evergram_bot_on_group_invite(bot, on_group_invite);
    evergram_bot_on_chat_removed(bot, on_chat_removed);
    evergram_bot_on_error(bot, on_error);
    evergram_bot_on_connected(bot, on_connected);
    evergram_bot_on_disconnected(bot, on_disconnected);
    evergram_bot_on_presence(bot, on_presence);
    evergram_bot_on_profile_updated(bot, on_profile_updated);
    return true;
}

void bridge_request_stop(bridge_t *bridge) {
    if (bridge == NULL) {
        return;
    }
    pthread_mutex_lock(&bridge->lock);
    bridge->stop = true;
    pthread_mutex_unlock(&bridge->lock);
}

/* Call only after the gateway thread has been joined. */
void bridge_destroy(bridge_t *bridge) {
    if (bridge == NULL) {
        return;
    }
    pthread_mutex_destroy(&bridge->lock);
    free(bridge);
}

size_t bridge_request_count(const bridge_t *bridge) {
    return bridge != NULL ? bridge->request_count : 0;
}

bool bridge_is_online(const bridge_t *bridge) {
    return bridge != NULL && bridge->online;
}

const char *bridge_identity(const bridge_t *bridge) {
    return bridge != NULL ? bridge->identity : "";
}
