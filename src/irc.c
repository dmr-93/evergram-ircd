#include "irc.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct irc_client {
    irc_server_t *server;
    int fd;

    char nick[IRC_NICK_MAX];
    char user[IRC_NICK_MAX];
    char realname[128];
    bool has_nick;
    bool has_user;
    bool registered;

    char inbuf[IRC_LINE_MAX * 2];
    size_t inlen;

    char channels[IRC_CHANNELS_PER_CLIENT][IRC_CHANNEL_MAX];
    size_t channel_count;

    irc_client_t *next;
};

typedef struct {
    irc_client_t *client;
    char line[IRC_LINE_MAX];
} queued_line_t;

struct irc_server {
    int listen_fd;
    irc_client_t *clients;
    size_t client_count;

    queued_line_t queue[IRC_QUEUED_LINES];
    size_t queue_head;
    size_t queue_count;
};

/* --- parsing --------------------------------------------------------------- */

void irc_upper(char *text) {
    for (; *text != '\0'; text++) {
        *text = (char)toupper((unsigned char)*text);
    }
}

const char *irc_channel_strip(const char *channel) {
    if (channel == NULL) {
        return "";
    }
    while (*channel == '#' || *channel == '&') {
        channel++;
    }
    return channel;
}

void irc_channel_name(const char *name, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return;
    }
    if (name == NULL || name[0] == '\0') {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_size, "%s%s", (name[0] == '#' || name[0] == '&') ? "" : "#", name);
}

bool irc_parse(const char *line, irc_message_t *out) {
    if (line == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const char *cursor = line;
    while (*cursor == ' ') {
        cursor++;
    }

    /* Optional prefix, which clients do not normally send but must be accepted. */
    if (*cursor == ':') {
        cursor++;
        const char *end = strchr(cursor, ' ');
        if (end == NULL) {
            return false;
        }
        size_t length = (size_t)(end - cursor);
        if (length >= sizeof(out->prefix)) {
            length = sizeof(out->prefix) - 1u;
        }
        memcpy(out->prefix, cursor, length);
        out->prefix[length] = '\0';
        cursor = end;
    }

    while (*cursor == ' ') {
        cursor++;
    }
    if (*cursor == '\0') {
        return false;
    }

    /* Command. */
    size_t used = 0;
    while (*cursor != '\0' && *cursor != ' ' && used + 1u < sizeof(out->command)) {
        out->command[used++] = *cursor++;
    }
    out->command[used] = '\0';

    /* Parameters, until the trailing starts. */
    while (*cursor != '\0') {
        while (*cursor == ' ') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        if (*cursor == ':') {
            cursor++;
            snprintf(out->trailing, sizeof(out->trailing), "%s", cursor);
            out->has_trailing = true;
            break;
        }

        const char *end = strchr(cursor, ' ');
        size_t length = end != NULL ? (size_t)(end - cursor) : strlen(cursor);
        if (out->param_count < IRC_MAX_PARAMS) {
            if (length >= IRC_TARGET_MAX) {
                length = IRC_TARGET_MAX - 1u;
            }
            memcpy(out->params[out->param_count], cursor, length);
            out->params[out->param_count][length] = '\0';
            out->param_count++;
        }
        cursor += length;
    }

    return out->command[0] != '\0';
}

/* --- writes ---------------------------------------------------------------- */

bool irc_send_raw(irc_client_t *client, const char *line) {
    if (client == NULL || client->fd < 0 || line == NULL) {
        return false;
    }

    char out[IRC_LINE_MAX + 2];
    int written = snprintf(out, sizeof(out) - 2u, "%s", line);
    if (written < 0) {
        return false;
    }
    out[written] = '\r';
    out[written + 1] = '\n';

    size_t total = (size_t)written + 2u;
    size_t sent = 0;
    while (sent < total) {
        ssize_t result = send(client->fd, out + sent, total - sent, MSG_NOSIGNAL);
        if (result <= 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += (size_t)result;
    }
    return true;
}

/*
 * IRC caps a line at 512 bytes including the CRLF, so anything longer is
 * truncated here rather than sent as a line a client will cut anyway. The
 * sends below build into a bounded buffer by hand for that reason: an Evergram
 * message can be far longer than a line allows.
 */
static size_t append_text(char *out, size_t limit, size_t used, const char *text) {
    if (text == NULL) {
        return used;
    }
    for (const char *cursor = text; *cursor != '\0' && used < limit; cursor++) {
        out[used++] = *cursor;
    }
    out[used] = '\0';
    return used;
}

void irc_send_from(irc_client_t *client, const char *from, const char *command, const char *target,
                   const char *text) {
    /* 510 bytes of payload, minus the CRLF irc_send_raw() adds. */
    char line[IRC_LINE_MAX];
    size_t limit = sizeof(line) - 1u;

    size_t used = 0;
    line[0] = '\0';
    used = append_text(line, limit, used, ":");
    used = append_text(line, limit, used, from);
    used = append_text(line, limit, used, " ");
    used = append_text(line, limit, used, command);
    used = append_text(line, limit, used, " ");
    used = append_text(line, limit, used, target);
    used = append_text(line, limit, used, " :");
    append_text(line, limit, used, text);

    irc_send_raw(client, line);
}

void irc_send_numeric(irc_client_t *client, int code, const char *rest) {
    char line[IRC_LINE_MAX];
    size_t limit = sizeof(line) - 1u;

    size_t used = 0;
    line[0] = '\0';
    used = append_text(line, limit, used, ":");
    used = append_text(line, limit, used, IRC_SERVER_NAME);
    char code_text[8];
    snprintf(code_text, sizeof(code_text), " %03d ", code);
    used = append_text(line, limit, used, code_text);
    used = append_text(line, limit, used, irc_client_nick(client));
    used = append_text(line, limit, used, " ");
    append_text(line, limit, used, rest);

    irc_send_raw(client, line);
}

void irc_notice(irc_client_t *client, const char *format, ...) {
    if (client == NULL) {
        return;
    }

    char text[IRC_TEXT_MAX];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    /* The target is the nick once registration happened, "*" before that. */
    const char *target = client->has_nick ? client->nick : "*";
    irc_send_from(client, IRC_SERVER_NAME, "NOTICE", target, text);
}

/* --- clients --------------------------------------------------------------- */

const char *irc_client_nick(const irc_client_t *client) {
    return client != NULL ? client->nick : "";
}

void irc_client_set_nick(irc_client_t *client, const char *nick) {
    if (client == NULL || nick == NULL) {
        return;
    }
    snprintf(client->nick, sizeof(client->nick), "%s", nick);
    client->has_nick = true;
}

void irc_client_set_user(irc_client_t *client, const char *user, const char *realname) {
    if (client == NULL) {
        return;
    }
    snprintf(client->user, sizeof(client->user), "%s", user != NULL ? user : "user");
    snprintf(client->realname, sizeof(client->realname), "%s",
             realname != NULL && realname[0] != '\0' ? realname : client->user);
    client->has_user = true;
}

const char *irc_client_user(const irc_client_t *client) {
    return client != NULL ? client->user : "";
}

const char *irc_client_realname(const irc_client_t *client) {
    return client != NULL ? client->realname : "";
}

bool irc_client_registered(const irc_client_t *client) {
    return client != NULL && client->registered;
}

void irc_client_mark_registered(irc_client_t *client) {
    if (client != NULL) {
        client->registered = true;
    }
}

const char *irc_client_host(const irc_client_t *client) {
    (void)client;
    return IRC_SERVER_NAME;
}

static void name_upper(const char *in, char *out, size_t out_size) {
    size_t used = 0;
    for (; *in != '\0' && used + 1u < out_size; in++) {
        out[used++] = (char)toupper((unsigned char)*in);
    }
    out[used] = '\0';
}

bool irc_client_add_channel(irc_client_t *client, const char *channel) {
    if (client == NULL || channel == NULL || channel[0] == '\0') {
        return false;
    }

    char wanted[IRC_CHANNEL_MAX];
    name_upper(channel, wanted, sizeof(wanted));
    for (size_t i = 0; i < client->channel_count; i++) {
        char existing[IRC_CHANNEL_MAX];
        name_upper(client->channels[i], existing, sizeof(existing));
        if (strcmp(existing, wanted) == 0) {
            return true; /* already joined; IRC treats this as a no-op */
        }
    }
    if (client->channel_count == IRC_CHANNELS_PER_CLIENT) {
        return false;
    }
    snprintf(client->channels[client->channel_count], IRC_CHANNEL_MAX, "%s", channel);
    client->channel_count++;
    return true;
}

void irc_client_remove_channel(irc_client_t *client, const char *channel) {
    if (client == NULL || channel == NULL) {
        return;
    }

    char wanted[IRC_CHANNEL_MAX];
    name_upper(channel, wanted, sizeof(wanted));
    for (size_t i = 0; i < client->channel_count; i++) {
        char existing[IRC_CHANNEL_MAX];
        name_upper(client->channels[i], existing, sizeof(existing));
        if (strcmp(existing, wanted) == 0) {
            client->channels[i][0] = '\0';
            /* Compact, so iteration order stays meaningful. */
            for (size_t j = i; j + 1u < client->channel_count; j++) {
                snprintf(client->channels[j], IRC_CHANNEL_MAX, "%s", client->channels[j + 1u]);
            }
            client->channel_count--;
            return;
        }
    }
}

bool irc_client_in_channel(const irc_client_t *client, const char *channel) {
    if (client == NULL || channel == NULL) {
        return false;
    }

    char wanted[IRC_CHANNEL_MAX];
    name_upper(channel, wanted, sizeof(wanted));
    for (size_t i = 0; i < client->channel_count; i++) {
        char existing[IRC_CHANNEL_MAX];
        name_upper(client->channels[i], existing, sizeof(existing));
        if (strcmp(existing, wanted) == 0) {
            return true;
        }
    }
    return false;
}

size_t irc_client_channel_count(const irc_client_t *client) {
    return client != NULL ? client->channel_count : 0;
}

const char *irc_client_channel_at(const irc_client_t *client, size_t index) {
    if (client == NULL || index >= client->channel_count) {
        return "";
    }
    return client->channels[index];
}

/* --- server ---------------------------------------------------------------- */

irc_server_t *irc_server_create(const char *host, int port, char *error, size_t error_size) {
    irc_server_t *server = calloc(1, sizeof(*server));
    if (server == NULL) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    server->listen_fd = -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(error, error_size, "socket: %s", strerror(errno));
        free(server);
        return NULL;
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    if (host == NULL || host[0] == '\0' || strcmp(host, "localhost") == 0) {
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
        snprintf(error, error_size, "not an IPv4 address: %s", host);
        close(fd);
        free(server);
        return NULL;
    }

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        snprintf(error, error_size, "bind %s:%d: %s", host != NULL ? host : "127.0.0.1", port,
                 strerror(errno));
        close(fd);
        free(server);
        return NULL;
    }
    if (listen(fd, IRC_CLIENTS_MAX) != 0) {
        snprintf(error, error_size, "listen: %s", strerror(errno));
        close(fd);
        free(server);
        return NULL;
    }

    /* Non-blocking, because the whole server is serviced from one loop that
     * also has to keep the Evergram socket alive. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    server->listen_fd = fd;
    return server;
}

void irc_server_destroy(irc_server_t *server) {
    if (server == NULL) {
        return;
    }
    while (server->clients != NULL) {
        irc_client_t *next = server->clients->next;
        if (server->clients->fd >= 0) {
            close(server->clients->fd);
        }
        free(server->clients);
        server->clients = next;
    }
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
    }
    free(server);
}

int irc_server_listen_fd(const irc_server_t *server) {
    return server != NULL ? server->listen_fd : -1;
}

size_t irc_server_accept(irc_server_t *server) {
    if (server == NULL) {
        return 0;
    }

    size_t accepted = 0;
    for (;;) {
        int fd = accept(server->listen_fd, NULL, NULL);
        if (fd < 0) {
            break; /* EAGAIN in practice */
        }
        if (server->client_count >= IRC_CLIENTS_MAX) {
            const char *busy = "ERROR :Too many connections\r\n";
            ssize_t ignored = write(fd, busy, strlen(busy));
            (void)ignored;
            close(fd);
            continue;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }

        irc_client_t *client = calloc(1, sizeof(*client));
        if (client == NULL) {
            close(fd);
            continue;
        }
        client->server = server;
        client->fd = fd;
        client->next = server->clients;
        server->clients = client;
        server->client_count++;
        accepted++;
    }
    return accepted;
}

static void queue_line(irc_server_t *server, irc_client_t *client, const char *line) {
    if (server->queue_count == IRC_QUEUED_LINES) {
        /* Do not grow: dropping the oldest would reorder the client's own
         * commands, so the newest is dropped and the client is told. */
        irc_notice(client, "*** Input overflow: a command was dropped. Slow down.");
        return;
    }
    size_t slot = (server->queue_head + server->queue_count) % IRC_QUEUED_LINES;
    server->queue[slot].client = client;
    snprintf(server->queue[slot].line, sizeof(server->queue[slot].line), "%s", line);
    server->queue_count++;
}

/* Consumes complete lines out of a client's buffer. */
static void consume_lines(irc_server_t *server, irc_client_t *client) {
    for (;;) {
        char *newline = memchr(client->inbuf, '\n', client->inlen);
        if (newline == NULL) {
            if (client->inlen == sizeof(client->inbuf)) {
                /* A line longer than any legal IRC message: drop the buffer
                 * rather than keep growing it. */
                client->inlen = 0;
            }
            return;
        }

        size_t length = (size_t)(newline - client->inbuf);
        char line[IRC_LINE_MAX];
        size_t copy = length < sizeof(line) - 1u ? length : sizeof(line) - 1u;
        memcpy(line, client->inbuf, copy);
        line[copy] = '\0';
        if (copy > 0 && line[copy - 1u] == '\r') {
            line[copy - 1u] = '\0';
        }

        size_t consumed = length + 1u;
        memmove(client->inbuf, client->inbuf + consumed, client->inlen - consumed);
        client->inlen -= consumed;

        if (line[0] != '\0') {
            queue_line(server, client, line);
        }
    }
}

void irc_server_pump(irc_server_t *server) {
    if (server == NULL) {
        return;
    }

    irc_client_t *client = server->clients;
    while (client != NULL) {
        irc_client_t *next = client->next;

        ssize_t got = recv(client->fd, client->inbuf + client->inlen,
                           sizeof(client->inbuf) - client->inlen, 0);
        if (got > 0) {
            client->inlen += (size_t)got;
            consume_lines(server, client);
        } else if (got == 0) {
            irc_server_drop_client(server, client);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            irc_server_drop_client(server, client);
        }

        client = next;
    }
}

bool irc_server_next_line(irc_server_t *server, irc_client_t **client_out, char *line,
                          size_t line_size) {
    if (server == NULL || server->queue_count == 0) {
        return false;
    }

    queued_line_t *queued = &server->queue[server->queue_head];
    server->queue_head = (server->queue_head + 1u) % IRC_QUEUED_LINES;
    server->queue_count--;

    if (client_out != NULL) {
        *client_out = queued->client;
    }
    if (line != NULL && line_size > 0) {
        snprintf(line, line_size, "%s", queued->line);
    }
    return true;
}

int irc_server_prepare_select(const irc_server_t *server, fd_set *read_fds) {
    if (server == NULL || read_fds == NULL) {
        return -1;
    }

    int highest = server->listen_fd;
    if (highest >= 0) {
        FD_SET(highest, read_fds);
    }
    for (irc_client_t *client = server->clients; client != NULL; client = client->next) {
        FD_SET(client->fd, read_fds);
        if (client->fd > highest) {
            highest = client->fd;
        }
    }
    return highest;
}

size_t irc_server_client_count(const irc_server_t *server) {
    return server != NULL ? server->client_count : 0;
}

irc_client_t *irc_server_client_at(const irc_server_t *server, size_t index) {
    if (server == NULL) {
        return NULL;
    }
    irc_client_t *client = server->clients;
    for (size_t i = 0; client != NULL && i < index; i++) {
        client = client->next;
    }
    return client;
}

size_t irc_server_registered_count(const irc_server_t *server) {
    size_t count = 0;
    for (irc_client_t *client = server != NULL ? server->clients : NULL; client != NULL;
         client = client->next) {
        if (client->registered) {
            count++;
        }
    }
    return count;
}

irc_client_t *irc_server_registered_at(const irc_server_t *server, size_t index) {
    if (server == NULL) {
        return NULL;
    }
    size_t seen = 0;
    for (irc_client_t *client = server->clients; client != NULL; client = client->next) {
        if (!client->registered) {
            continue;
        }
        if (seen == index) {
            return client;
        }
        seen++;
    }
    return NULL;
}

void irc_server_drop_client(irc_server_t *server, irc_client_t *client) {
    if (server == NULL || client == NULL) {
        return;
    }

    irc_client_t **link = &server->clients;
    while (*link != NULL && *link != client) {
        link = &(*link)->next;
    }
    if (*link == NULL) {
        return;
    }
    *link = client->next;
    server->client_count--;

    if (client->fd >= 0) {
        close(client->fd);
    }

    /* Any queued line from this client is dropped with it: delivering it to a
     * freed client would be worse than losing it. */
    size_t kept = 0;
    for (size_t i = 0; i < server->queue_count; i++) {
        size_t slot = (server->queue_head + i) % IRC_QUEUED_LINES;
        if (server->queue[slot].client == client) {
            continue;
        }
        size_t target = (server->queue_head + kept) % IRC_QUEUED_LINES;
        server->queue[target] = server->queue[slot];
        kept++;
    }
    server->queue_count = kept;

    free(client);
}
