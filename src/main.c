/*
 * evergram-irc — a local IRC server that is really your Evergram account.
 *
 * Start it, point any IRC client at localhost:6667, and the standard IRC
 * commands become Evergram operations: /LIST shows your chats, /MSG writes into
 * one, /WHOIS shows the identity behind a nick, and the events Evergram pushes
 * (messages, edits, reactions, join requests, invites) arrive as PRIVMSGs and
 * NOTICEs.
 *
 * Everything runs in one thread and one loop: the IRC sockets are serviced with
 * select() and the Evergram socket with the SDK's poll, never simultaneously,
 * which is what keeps the SDK's "no calls from callbacks" rule easy to honour —
 * commands that need a gateway round trip are queued and run from the loop.
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "bridge.h"
#include "evergram.h"
#include "irc.h"

#define DEFAULT_GATEWAY "wss://staging.evergram.app/api/ws"
#define POLL_INTERVAL_MS 40

static volatile sig_atomic_t g_running = 1;

static void on_signal(int number) {
    (void)number;
    g_running = 0;
}

typedef struct {
    const char *gateway;
    const char *identity_path;
    const char *host;
    int port;
    const char *name;
} options_t;

static void usage(const char *program) {
    printf("usage: %s [options]\n\n", program);
    printf("A local IRC server (port %d) that speaks Evergram on the other side.\n\n",
           IRC_DEFAULT_PORT);
    printf("  --gateway <url>     Evergram gateway (default %s,\n", DEFAULT_GATEWAY);
    printf("                      or $EVERGRAM_GATEWAY_URL)\n");
    printf("  --identity <path>   identity file, created on first run\n");
    printf("                      (default %s, or $EVERGRAM_IDENTITY_FILE)\n",
           "evergram-irc-identity.json");
    printf("  --name <nickname>   profile name to set on the Evergram account\n");
    printf("  --host <address>    bind address (default 127.0.0.1)\n");
    printf("  --port <port>       listen port (default %d)\n", IRC_DEFAULT_PORT);
    printf("  --print-peer-nick <identity>      the IRC nick an identity gets\n");
    printf("  --print-nick-from-name <nickname> the IRC nick an Evergram nickname becomes\n");
    printf("  --help              this text\n\n");
    printf("Then, from an IRC client:\n");
    printf("  /server 127.0.0.1 %d     /nick <your nick>\n", IRC_DEFAULT_PORT);
    printf("  /list                    /help     /users     /whois <nick>\n");
}

static const char *option_or_env(const char *value, const char *env, const char *fallback) {
    if (value != NULL && value[0] != '\0') {
        return value;
    }
    const char *from_env = getenv(env);
    if (from_env != NULL && from_env[0] != '\0') {
        return from_env;
    }
    return fallback;
}

/* Prints the nick a peer identity gets, and exits. The mapping is the kind of
 * thing that is easier to check than to reason about. */
static int print_peer_nick(const char *identity) {
    char nick[IRC_NICK_MAX];
    bridge_peer_nick(identity, nick, sizeof(nick));
    printf("%s\n", nick);
    return EXIT_SUCCESS;
}

static bool parse_options(int argc, char **argv, options_t *options) {
    memset(options, 0, sizeof(*options));
    options->port = IRC_DEFAULT_PORT;
    options->host = "127.0.0.1";

    for (int i = 1; i < argc; i++) {
        const char *argument = argv[i];
        const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0) {
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        }
        if (strcmp(argument, "--print-peer-nick") == 0) {
            exit(print_peer_nick(value));
        }
        if (strcmp(argument, "--print-nick-from-name") == 0) {
            char nick[IRC_NICK_MAX];
            bridge_sanitize_nickname(value, nick, sizeof(nick));
            printf("%s\n", nick);
            exit(EXIT_SUCCESS);
        }
        if (value == NULL) {
            fprintf(stderr, "[evergram-irc] %s needs a value\n", argument);
            return false;
        }

        if (strcmp(argument, "--gateway") == 0) {
            options->gateway = value;
        } else if (strcmp(argument, "--identity") == 0) {
            options->identity_path = value;
        } else if (strcmp(argument, "--name") == 0) {
            options->name = value;
        } else if (strcmp(argument, "--host") == 0) {
            options->host = value;
        } else if (strcmp(argument, "--port") == 0) {
            long port = strtol(value, NULL, 10);
            if (port <= 0 || port > 65535) {
                fprintf(stderr, "[evergram-irc] invalid port: %s\n", value);
                return false;
            }
            options->port = (int)port;
        } else {
            fprintf(stderr, "[evergram-irc] unknown option: %s\n", argument);
            return false;
        }
        i++;
    }

    options->gateway = option_or_env(options->gateway, "EVERGRAM_GATEWAY_URL", DEFAULT_GATEWAY);
    options->identity_path =
        option_or_env(options->identity_path, "EVERGRAM_IDENTITY_FILE", "evergram-irc-identity.json");
    options->name = option_or_env(options->name, "EVERGRAM_NAME", NULL);
    return true;
}

/*
 * The gateway thread. Everything the SDK is asked to do happens here, including
 * the poll that can block for seconds while a connection is retried, so the IRC
 * side never waits for Evergram.
 */
static void *gateway_thread(void *argument) {
    bridge_t *bridge = argument;
    while (bridge_gateway_step(bridge)) {
        /* bridge_gateway_step() does one poll and at most one queued request. */
    }
    return NULL;
}

int main(int argc, char **argv) {
    options_t options;
    if (!parse_options(argc, argv, &options)) {
        return EXIT_FAILURE;
    }

    const char *level = getenv("EVERGRAM_LOG");
    if (level != NULL) {
        evergram_log_set_level(evergram_log_level_from_name(level));
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    /* A client that goes away mid-write must not kill the bridge. */
    signal(SIGPIPE, SIG_IGN);

    char error[256];
    error[0] = '\0';
    irc_server_t *server = irc_server_create(options.host, options.port, error, sizeof(error));
    if (server == NULL) {
        fprintf(stderr, "[evergram-irc] cannot listen on %s:%d: %s\n", options.host, options.port,
                error);
        return EXIT_FAILURE;
    }

    /* The bridge is created first so it can be the bot's user_data: that is how
     * every Evergram callback finds its way back to the IRC side. */
    bridge_t *bridge = bridge_create(server, options.name);
    if (bridge == NULL) {
        fprintf(stderr, "[evergram-irc] cannot create the bridge\n");
        irc_server_destroy(server);
        return EXIT_FAILURE;
    }

    /* The bot layer owns the identity (creating and saving one on first run),
     * the reconnection and the mailbox, so the bridge only has to sequence. */
    const evergram_bot_options_t bot_options = {
        .url = options.gateway,
        .identity_path = options.identity_path,
        .name = options.name,
        .platform = "Terminal",
        .user_data = bridge,
    };

    evergram_bot_t *bot = evergram_bot_create(&bot_options);
    if (bot == NULL) {
        fprintf(stderr, "[evergram-irc] cannot create the Evergram account\n");
        bridge_destroy(bridge);
        irc_server_destroy(server);
        return EXIT_FAILURE;
    }

    if (!bridge_attach(bridge, bot)) {
        fprintf(stderr, "[evergram-irc] cannot attach the bridge\n");
        evergram_bot_destroy(bot);
        bridge_destroy(bridge);
        irc_server_destroy(server);
        return EXIT_FAILURE;
    }

    printf("[evergram-irc] IRC listening on %s:%d\n", options.host, options.port);
    printf("[evergram-irc] Evergram gateway %s, identity %s\n", options.gateway,
           options.identity_path);
    printf("[evergram-irc] point your IRC client at %s %d (nick and USER are yours to pick)\n",
           options.host, options.port);

    evergram_status_t status = evergram_bot_start(bot);
    if (status != EVERGRAM_OK) {
        /* The IRC side still comes up: a client that connects while the gateway
         * is unreachable gets told so, instead of finding nothing listening. */
        fprintf(stderr, "[evergram-irc] gateway connection failed (%s); retrying in the "
                        "background\n",
                evergram_status_str(status));
    }

    pthread_t gateway;
    if (pthread_create(&gateway, NULL, gateway_thread, bridge) != 0) {
        fprintf(stderr, "[evergram-irc] cannot start the gateway thread\n");
        evergram_bot_destroy(bot);
        bridge_destroy(bridge);
        irc_server_destroy(server);
        return EXIT_FAILURE;
    }

    while (g_running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        int highest = irc_server_prepare_select(server, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = POLL_INTERVAL_MS * 1000;

        int ready = select(highest + 1, &read_fds, NULL, NULL, &timeout);
        if (ready > 0) {
            int listen_fd = irc_server_listen_fd(server);
            if (listen_fd >= 0 && FD_ISSET(listen_fd, &read_fds)) {
                irc_server_accept(server);
            }
        } else if (ready < 0 && errno != EINTR) {
            fprintf(stderr, "[evergram-irc] select: %s\n", strerror(errno));
            break;
        }

        irc_server_pump(server);

        irc_client_t *client = NULL;
        char line[IRC_LINE_MAX];
        size_t handled = 0;
        while (handled < 16 && irc_server_next_line(server, &client, line, sizeof(line))) {
            bridge_handle_line(bridge, client, line);
            handled++;
        }

        /* Hand the IRC side whatever the gateway produced. Nothing here ever
         * waits for the SDK: that is the gateway thread's job. */
        bridge_flush(bridge);
    }

    printf("\n[evergram-irc] shutting down\n");

    /* Order matters: the gateway thread is still inside the SDK, so it is asked
     * to stop and waited for before anything it uses is freed. A blocking poll
     * finishes in at most one transport timeout. */
    bridge_request_stop(bridge);
    pthread_join(gateway, NULL);
    bridge_destroy(bridge);
    evergram_bot_destroy(bot);
    irc_server_destroy(server);
    return EXIT_SUCCESS;
}
