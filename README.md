# evergram-irc

A local IRC server that is really your Evergram account.

Start it, point any IRC client at `127.0.0.1:6667`, and the commands you already
know become Evergram operations. Your chats show up as channels, identities as
nicks, and everything Evergram pushes at you — messages, edits, reactions, join
requests, invites — arrives as a `PRIVMSG` or a `NOTICE` in your client.

```
$ ./build/evergram-irc
[evergram-irc] IRC listening on 127.0.0.1:6667
[evergram-irc] Evergram gateway wss://staging.evergram.app/api/ws, identity evergram-irc-identity.json
```

Then, in irssi / WeeChat / HexChat / whatever you have:

```
/server 127.0.0.1 6667
/nick you
/list          → your chats, one channel each
/msg r3VfWw2 hi → send into that chat (creates it if it does not exist yet)
/whois you     → the account behind this bridge: EVERGRAM
```

## What maps to what

| IRC | Evergram |
| --- | --- |
| channel `#<chat id>` | a chat (group or one-on-one) |
| nick | the peer's **Evergram nickname** when it has one (`Demis Rosseti` → `DemisRosseti`), otherwise its identity key truncated to 20 characters (`rNPvaf8QNuUFh9xoRTj4`); `/whois` always shows the full identity |
| `PRIVMSG` | a message in that chat |
| `NOTICE` | edits, deletions, reactions, presence, errors |
| `/list` | your chats |
| `/join #<chat>` | open a chat; `/join <code>` asks to join by invite code |
| `/part` | leave a group (a one-on-one just closes the window) |
| `/names`, `/who`, `/topic` | participants and chat metadata |
| `/whois <nick>` | the identity, and the chats you share with it |
| `/users` | every identity seen so far |
| `/approve`, `/deny` | approve or deny a join request |
| `/accept`, `/decline` | accept or decline a chat request |
| `/gjoin` | accept a group invite |
| `/invite`, `/remove` | add or remove someone in a chat |

`/help` prints the same list from inside the client.

**`/nick` renames you on Evergram**, not just in the client: the profile nickname
is what other people see, so the two are kept in step (a change made while the
gateway is down is queued and applied when it returns). If you start the bridge
with `--name`, be aware that this configured name is re-applied on every
reconnect and would overwrite a later `/nick`.

## How it is put together

One Evergram account, two threads:

```
  IRC client ──TCP──▶  IRC thread  ──┐ snapshot + request queue ─┐
   (your client)        (select)     │        one mutex          │
                                      └──▶ gateway thread ───────┘
                                             (SDK: poll/send/…)
```

The gateway thread is not an optimisation: the SDK's `evergram_poll()` **blocks
for seconds** while it retries a connection (measured: 4.7 s, then 24.9 s on a
dead gateway), and an IRC client that freezes every time the network hiccups is
unusable. With the split, an unreachable gateway costs nothing on the IRC side —
the smoke test runs entirely with the gateway down.

Every SDK call happens on the gateway thread, so the SDK stays single-threaded
as it requires. The two threads share only a chat/peer snapshot and two queues,
all guarded by one mutex, and queued lines are addressed by nick rather than by
a client pointer, so a client that disconnects cannot leave a dangling
reference. `-fsanitize=thread` reports no races on a full session.

## Options

```
--gateway <url>    Evergram gateway (default wss://staging.evergram.app/api/ws,
                   or $EVERGRAM_GATEWAY_URL)
--identity <path>  identity file, created on first run with mode 0600
                   (default evergram-irc-identity.json, or $EVERGRAM_IDENTITY_FILE)
--name <nickname>  profile name to set on the account (or $EVERGRAM_NAME)
--host <address>   bind address, default 127.0.0.1
--port <port>      default 6667
```

`EVERGRAM_LOG=debug` turns on the SDK's own logging, which is where to look when
the connection misbehaves.

## Building

Needs the SDK built next door (the default `EVERGRAM_DIR` is `../evergram-sdk-c`):

```sh
cd ../evergram-sdk-c && make          # the library this links against
cd ../evergram-irc && make            # build/evergram-irc
make test                             # IRC protocol smoke test, no gateway needed
```

## Known limits (this first version)

* **One account.** Every IRC client that connects is the same Evergram account,
  with its own nick; there is no per-nick identity.
* **No public chat discovery.** `/list` shows *your* chats. The SDK has no
  wrapper for the protocol's public-chat query yet, so there is nothing to
  discover; `/join <invite code>` works.
* **IRC has no edits, reactions or typing.** Those arrive as `NOTICE`s, and
  typing is dropped on purpose because the signals are constant.
* **A long message is truncated** to fit one IRC line (510 bytes), which is a
  protocol limit, not a choice.
* **Nicks are the Evergram nickname when there is one.** The bridge fetches each
  peer's profile once, keeps the nickname as the nick, and announces a rename as
  a proper IRC `NICK` change if the profile changes. A nickname that sanitizes to
  nothing (or a peer with no profile) falls back to the address, cut to 20
  characters because an XRPL address runs up to 35 and an IRC nick is capped
  around 31; two peers ending up with the same nick get a numeric suffix, and
  `/whois` always shows the whole identity.
  `./build/evergram-irc --print-peer-nick 1:r…` and `--print-nick-from-name
  "Some Name"` print both mappings.
* **Topics, modes and kicks are read-only or best-effort**: Evergram has no
  topic or mode, so `/topic` shows chat metadata and `/mode` is a stub.
* **No TLS on the IRC side** and it binds to loopback by default: this is a
  local bridge for your own machine, not a public server.
* **`/LIST` reflects the chats Evergram has told us about.** An empty list asks
  Evergram to refresh and says so, rather than leaving you staring at a blank
  window; `/STATUS` reports how many chats are known.
* **A reply goes back into the conversation the message came from** — the chat
  the peer actually wrote in — never into a new one that merely looks similar.
  `evergram-irc` resolves that against the SDK itself, not against its own view
  of the chat list, because that view can be a moment behind.
* **Your own messages are not echoed back**, as in any IRC server: your client
  shows what you typed. With a bare `telnet` that means the line you send is not
  printed again — `/STATUS` and the bridge's own log are how you confirm it went
  out, and a send that fails arrives as a `NOTICE`.

## Layout

```
include/irc.h      the IRC slice: parsing, replies, clients, server
include/bridge.h   the Evergram <-> IRC boundary and its thread model
src/irc.c          socket handling, line parsing, numerics
src/bridge.c       the mapping, the command table, the queues
src/main.c         options, the two threads, the single-threaded IRC loop
tests/irc_smoke.py drives a real session over TCP and checks the replies
```
