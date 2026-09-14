#ifndef EVERGRAM_IRC_BRIDGE_H
#define EVERGRAM_IRC_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>

#include "evergram.h"
#include "irc.h"

/*
 * Evergram <-> IRC.
 *
 * The bridge owns one Evergram account (the bot layer's identity) and presents
 * it to IRC clients as an ordinary network. The mapping is deliberately thin:
 *
 *   Evergram chat      ->  IRC channel  "#<chat id>"
 *   Evergram identity  ->  IRC nick     "r" + characters of the address
 *   Evergram message   ->  PRIVMSG      from "nick!identity@evergram"
 *   everything else    ->  NOTICE       (edits, reactions, presence, ...)
 *
 * Two threads, because the SDK's poll can block for seconds while it retries a
 * connection, and an IRC client must not feel that:
 *
 *   the IRC thread      reads sockets, answers from a snapshot, and queues
 *                       requests;
 *   the gateway thread  calls the SDK - poll, sends, everything - and queues
 *                       the lines to deliver.
 *
 * Only the queues, the chat/peer snapshot and the flags are shared, all under
 * one mutex, and every line is addressed by nick rather than by a client
 * pointer, so a client that disconnects cannot leave a dangling reference
 * behind. Every SDK call happens on the gateway thread, which is what keeps the
 * SDK's single-threaded contract intact.
 */

typedef struct bridge bridge_t;

/*
 * Two steps, because the bot's user_data has to be the bridge and the bridge
 * needs the bot: create the bridge, hand it to the bot as user_data, then
 * attach. Until attached, no Evergram event reaches IRC.
 */
bridge_t *bridge_create(irc_server_t *server, const char *nickname);
bool bridge_attach(bridge_t *bridge, evergram_bot_t *bot);

/*
 * Shutdown is deliberately two steps: ask the gateway thread to stop, join it,
 * and only then destroy. Freeing the bridge (or its mutex) while the gateway
 * thread is still inside bridge_gateway_step() is a use-after-free.
 */
void bridge_request_stop(bridge_t *bridge);
void bridge_destroy(bridge_t *bridge);

/* --- IRC thread ----------------------------------------------------------- */

/* Handles one line from a client. Never calls the SDK. */
void bridge_handle_line(bridge_t *bridge, irc_client_t *client, const char *line);

/* Writes every queued line to its destination. Call once per loop iteration. */
void bridge_flush(bridge_t *bridge);

/* --- gateway thread ------------------------------------------------------- */

/*
 * One step: services the Evergram socket (which may block while reconnecting),
 * executes at most one queued request, and refreshes the snapshot when the chat
 * list changed. Returns false when the bridge was destroyed.
 */
bool bridge_gateway_step(bridge_t *bridge);

/* Refreshes the chat/peer snapshot from the SDK. Gateway thread only. */
void bridge_refresh_snapshot(bridge_t *bridge);

/*
 * The IRC nick a peer is known by: the address part of its identity key,
 * truncated to fit an IRC nick. Exposed because it is pure, and the first thing
 * to check when a nick looks wrong.
 */
void bridge_peer_nick(const char *identity, char *out, size_t out_size);

/*
 * Turns an Evergram profile nickname into an IRC nick: letters, digits, '-' and
 * '_' only, not starting with a digit, short enough for a collision suffix. An
 * empty result means "no usable nickname", and the peer keeps its address nick.
 */
void bridge_sanitize_nickname(const char *nickname, char *out, size_t out_size);

size_t bridge_request_count(const bridge_t *bridge);
bool bridge_is_online(const bridge_t *bridge);
const char *bridge_identity(const bridge_t *bridge);

#endif /* EVERGRAM_IRC_BRIDGE_H */
