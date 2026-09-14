#!/usr/bin/env python3
"""Drives the bridge over TCP and checks what an IRC client would see.

No gateway is involved: the bridge is expected to serve IRC whether or not the
Evergram connection is up, which is exactly the state this test runs in. What is
checked here is the IRC side — registration, the welcome burst, the commands a
client actually sends, and the Evergram-specific ones — so a regression in the
protocol layer fails fast without touching the network.
"""

import os
import socket
import subprocess
import sys
import time

PORT = 16667  # not 6667, so a real bridge can run at the same time
HOST = "127.0.0.1"

failures = []
checks = 0


def check(condition, description, detail=""):
    global checks
    checks += 1
    if not condition:
        failures.append(f"{description}{': ' + detail if detail else ''}")
        print(f"    FAIL {description}{' -> ' + detail if detail else ''}")
    else:
        print(f"    ok   {description}")


class Irc:
    def __init__(self, port):
        self.sock = socket.create_connection((HOST, port), timeout=5)
        self.sock.settimeout(2)
        self.buffer = b""

    def send(self, line):
        self.sock.sendall(line.encode() + b"\r\n")

    def lines(self, seconds=0.6):
        """Everything received within the window, split into lines."""
        deadline = time.time() + seconds
        out = []
        while time.time() < deadline:
            self.sock.settimeout(max(0.05, deadline - time.time()))
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            self.buffer += chunk
            while b"\r\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\r\n", 1)
                out.append(line.decode("utf-8", "replace"))
        return out

    def wait_for(self, needle, seconds=2.0):
        deadline = time.time() + seconds
        collected = []
        while time.time() < deadline:
            collected += self.lines(0.2)
            if any(needle in line for line in collected):
                return collected
        return collected

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "build/evergram-irc"
    if not os.path.exists(binary):
        print(f"binary not found: {binary}")
        return 1

    env = dict(os.environ)
    # A gateway that refuses instantly: the bridge must still serve IRC.
    env["EVERGRAM_GATEWAY_URL"] = "ws://127.0.0.1:1/api/ws"
    env["EVERGRAM_IDENTITY_FILE"] = os.path.join(
        os.path.dirname(binary), "test-identity.json"
    )
    process = subprocess.Popen(
        [binary, "--port", str(PORT), "--identity", env["EVERGRAM_IDENTITY_FILE"]],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    print("  peer nick mapping")
    import subprocess as sp

    def peer_nick(identity):
        return sp.run([binary, "--print-peer-nick", identity], capture_output=True, text=True).stdout.strip()

    check(
        peer_nick("1:rNPvaf8QNuUFh9xoRTj48BdoodWSywywdw") == "rNPvaf8QNuUFh9xoRTj4",
        "a peer nick is the address, not r+address",
        peer_nick("1:rNPvaf8QNuUFh9xoRTj48BdoodWSywywdw"),
    )
    check(
        peer_nick("1:rMibrQV7rCNq5bMkaWx9wZkF9vt23fw2yB").startswith("rMibrQV7rCNq5bMkaWx9"),
        "the address itself is kept, truncated to fit a nick",
        peer_nick("1:rMibrQV7rCNq5bMkaWx9wZkF9vt23fw2yB"),
    )
    check(len(peer_nick("1:rNPvaf8QNuUFh9xoRTj48BdoodWSywywdw")) <= 20, "a nick stays short")
    check(peer_nick("") == "", "an empty identity yields no nick")

    def nick_from_name(name):
        return sp.run(
            [binary, "--print-nick-from-name", name], capture_output=True, text=True
        ).stdout.strip()

    check(nick_from_name("Demis") == "Demis", "a plain nickname is kept")
    check(nick_from_name("Demis Rosseti") == "DemisRosseti", "spaces are dropped")
    check(nick_from_name("demis.rosseti") == "demisrosseti", "punctuation is dropped")
    check(nick_from_name("123abc") == "_123abc", "a nick may not start with a digit")
    check(nick_from_name("") == "", "an empty nickname yields no nick")
    check(
        nick_from_name("a-very-long-nickname-that-goes-on") == "a-very-long-nickname",
        "a long nickname is truncated for a collision suffix to fit",
    )

    try:
        # Wait for the listener.
        for _ in range(60):
            try:
                socket.create_connection((HOST, PORT), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.1)
        else:
            print("the bridge never started listening")
            return 1

        print("  registration")
        client = Irc(PORT)
        client.send("NICK tester")
        client.send("USER tester 0 * :Test Person")
        welcome = client.wait_for("001 tester")
        joined = "\n".join(welcome)
        check("001 tester" in joined, "001 welcome is sent")
        check("EVERGRAM" in joined, "the welcome names the network EVERGRAM", joined[:120])
        check("002 tester" in joined, "002 your host is sent")
        check("005 tester" in joined, "005 ISUPPORT is sent")
        check("NETWORK=EVERGRAM" in joined, "ISUPPORT advertises the network")
        check("376 tester" in joined, "MOTD ends")

        print("  registration details")
        early = Irc(PORT)
        early.send("NICK early")
        noise = "\n".join(early.lines(0.4))
        check(
            "NOTICE" not in noise,
            "a lone NICK is not answered with a complaint",
            noise.strip()[:80],
        )
        early.send("USER early 0 * :Early")
        check("001 early" in "\n".join(early.wait_for("001 early")), "registration then proceeds")
        early.close()

        print("  client niceties")
        caps = Irc(PORT)
        caps.send("CAP LS 302")
        answer = "\n".join(caps.wait_for("CAP"))
        check("CAP * LS" in answer, "CAP LS is answered (HexChat waits for it)", answer[:80])
        caps.send("NICK caps")
        caps.send("USER caps 0 * :Caps")
        caps.wait_for("001 caps")
        caps.send("MOTD")
        check("376 caps" in "\n".join(caps.wait_for("376")), "/MOTD answers")
        caps.send("USERHOST caps")
        check("302 caps" in "\n".join(caps.wait_for("302")), "/USERHOST answers")
        caps.send("ISON caps")
        check("303 caps" in "\n".join(caps.wait_for("303")), "/ISON answers")
        caps.send("LUSERS")
        check("251 caps" in "\n".join(caps.wait_for("251")), "/LUSERS answers")
        caps.close()

        print("  liveness")
        client.send("PING :token123")
        check(
            any("PONG" in line and "token123" in line for line in client.wait_for("PONG")),
            "PING is answered with PONG",
        )

        print("  standard commands")
        client.send("LIST")
        listed = "\n".join(client.wait_for("323"))
        check("321 tester" in listed, "/LIST opens with 321")
        check("323 tester" in listed, "/LIST closes with 323")

        client.send("MODE tester")
        check(
            any(" 221 tester" in line for line in client.wait_for("221")),
            "/MODE on a nick answers 221",
        )

        client.send("WHOIS tester")
        whois = "\n".join(client.wait_for("318"))
        check(" 311 tester" in whois, "/WHOIS answers 311")
        check("EVERGRAM" in whois, "WHOIS shows EVERGRAM as the network", whois[:160])

        client.send("USERS")
        check(
            any("Evergram identities" in line for line in client.wait_for("identities")),
            "/USERS answers",
        )

        client.send("STATUS")
        status = "\n".join(client.wait_for("Reconnect attempts"))
        check("Evergram connection:" in status, "/STATUS reports the connection")
        check("waiting for their chat key:" in status, "/STATUS reports the mailbox depth")
        check("Account:" in status, "/STATUS reports the account")

        client.send("HELP")
        check(
            any("/LIST" in line for line in client.wait_for("/LIST")),
            "/HELP lists the command map",
        )

        client.send("FOOBARBAZ")
        check(
            any(" 421 tester" in line for line in client.wait_for("421")),
            "an unknown command answers 421",
        )

        client.send("JOIN #not-a-chat")
        check(
            any(" 403 tester" in line for line in client.wait_for("403")),
            "/JOIN on an unknown channel answers 403",
        )

        client.send("PRIVMSG #not-a-chat :hello")
        check(
            any(" 403 tester" in line for line in client.wait_for("403")),
            "/MSG to an unknown channel answers 403",
        )

        client.send("PRIVMSG nosuchnick :hello")
        check(
            any(" 401 tester" in line for line in client.wait_for("401")),
            "/MSG to an unknown nick answers 401",
        )

        print("  shutdown")
        client.send("QUIT :bye")
        client.close()

        # The server must survive a client that vanishes without QUIT.
        rude = Irc(PORT)
        rude.send("NICK rude")
        rude.send("USER rude 0 * :Rude")
        rude.wait_for("001 rude")
        rude.sock.close()
        time.sleep(0.2)

        survivor = Irc(PORT)
        survivor.send("NICK survivor")
        survivor.send("USER survivor 0 * :Survivor")
        check(
            "001 survivor" in "\n".join(survivor.wait_for("001 survivor")),
            "a new client still registers after an abrupt disconnect",
        )
        survivor.close()

    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
        if os.path.exists(env["EVERGRAM_IDENTITY_FILE"]):
            os.remove(env["EVERGRAM_IDENTITY_FILE"])

    print()
    print(f"{checks - len(failures)}/{checks} checks passed")
    if failures:
        for failure in failures:
            print(f"  failed: {failure}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
