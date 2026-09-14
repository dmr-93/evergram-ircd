#ifndef EVERGRAM_IRC_IRC_H
#define EVERGRAM_IRC_IRC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/select.h>

/*
 * The slice of IRC this bridge speaks.
 *
 * Enough of RFC 1459/2812 for a normal client to register, list, join, talk and
 * ask who someone is — plus a handful of Evergram-specific commands the client
 * passes through as ordinary commands (USERS, APPROVE, ACCEPT, ...). Anything
 * the protocol does not cover is answered with a NOTICE rather than silence, so
 * the user is never left guessing.
 *
 * The server name is "evergram"; the network it presents itself as is
 * "EVERGRAM", which is what the welcome burst and WHOIS show.
 */

#define IRC_SERVER_NAME "evergram"
#define IRC_NETWORK_NAME "EVERGRAM"
#define IRC_VERSION "evergram-irc-0.1"

#define IRC_LINE_MAX 512
#define IRC_NICK_MAX 32
#define IRC_CHANNEL_MAX 128
#define IRC_TARGET_MAX 160
#define IRC_TEXT_MAX 4096
/* A topic or a names list is a summary and never a message body. */
#define IRC_TOPIC_MAX 320
#define IRC_MAX_PARAMS 15
#define IRC_CLIENTS_MAX 16
#define IRC_CHANNELS_PER_CLIENT 32
#define IRC_QUEUED_LINES 64

#define IRC_DEFAULT_PORT 6667

typedef struct irc_client irc_client_t;
typedef struct irc_server irc_server_t;

/*
 * One parsed IRC message: ":<prefix> COMMAND <params> :<trailing>".
 * `params` excludes the trailing, which is kept separately because it may
 * contain spaces.
 */
typedef struct {
    char prefix[IRC_NICK_MAX * 2 + 32];
    char command[32];
    char params[IRC_MAX_PARAMS][IRC_TARGET_MAX];
    size_t param_count;
    char trailing[IRC_TEXT_MAX];
    bool has_trailing;
} irc_message_t;

/* Splits one line into its parts. False when there is no command at all. */
bool irc_parse(const char *line, irc_message_t *out);

/* "chan" -> "#chan" (an already-prefixed name is left alone). */
void irc_channel_name(const char *name, char *out, size_t out_size);

/* The channel name without its leading '#' / '&'. */
const char *irc_channel_strip(const char *channel);

/* Uppercases in place, for case-insensitive command comparisons. */
void irc_upper(char *text);

/* --- writes ---------------------------------------------------------------- */

/* Raw line, with the terminating CRLF. False when the peer is gone. */
bool irc_send_raw(irc_client_t *client, const char *line);

/* ":<from> <command> <target> :<text>" */
void irc_send_from(irc_client_t *client, const char *from, const char *command,
                   const char *target, const char *text);

/* Numeric reply: ":evergram <code> <nick> <rest>" */
void irc_send_numeric(irc_client_t *client, int code, const char *rest);

/* A NOTICE from the server, which is how everything that is not a chat message
 * reaches the user. */
void irc_notice(irc_client_t *client, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

/* --- server ---------------------------------------------------------------- */

/* Creates the listening socket. A NULL host binds 127.0.0.1 only: a bridge that
 * exposes a whole Evergram account should not be reachable by accident. */
irc_server_t *irc_server_create(const char *host, int port, char *error, size_t error_size);
void irc_server_destroy(irc_server_t *server);

/* The listening socket, for the loop's readiness check. */
int irc_server_listen_fd(const irc_server_t *server);

/* Accepts every pending connection. Returns how many were accepted. */
size_t irc_server_accept(irc_server_t *server);

/*
 * Reads from every client socket and queues the complete lines. Disconnected
 * clients are dropped here. Call once per loop iteration; sockets are
 * non-blocking, so this never waits.
 */
void irc_server_pump(irc_server_t *server);

/* Pops one queued line, oldest first. False when nothing is queued. */
bool irc_server_next_line(irc_server_t *server, irc_client_t **client_out, char *line,
                          size_t line_size);

/* Fills `read_fds` for select() and returns the highest fd, or -1 when there is
 * nothing to wait for. */
int irc_server_prepare_select(const irc_server_t *server, fd_set *read_fds);

size_t irc_server_client_count(const irc_server_t *server);
irc_client_t *irc_server_client_at(const irc_server_t *server, size_t index);

/* Every registered client, so events can be broadcast to all of them. */
size_t irc_server_registered_count(const irc_server_t *server);
irc_client_t *irc_server_registered_at(const irc_server_t *server, size_t index);

/* Drops a client: closes the socket and frees it. */
void irc_server_drop_client(irc_server_t *server, irc_client_t *client);

/* --- client ---------------------------------------------------------------- */

const char *irc_client_nick(const irc_client_t *client);
void irc_client_set_nick(irc_client_t *client, const char *nick);
void irc_client_set_user(irc_client_t *client, const char *user, const char *realname);
const char *irc_client_user(const irc_client_t *client);
const char *irc_client_realname(const irc_client_t *client);
bool irc_client_registered(const irc_client_t *client);
void irc_client_mark_registered(irc_client_t *client);

/* The host shown in prefixes, always "evergram". */
const char *irc_client_host(const irc_client_t *client);

/* Channels this client has joined, as "#name" strings. */
bool irc_client_add_channel(irc_client_t *client, const char *channel);
void irc_client_remove_channel(irc_client_t *client, const char *channel);
bool irc_client_in_channel(const irc_client_t *client, const char *channel);
size_t irc_client_channel_count(const irc_client_t *client);
const char *irc_client_channel_at(const irc_client_t *client, size_t index);

#endif /* EVERGRAM_IRC_IRC_H */
