"""Talk to the running Gothic 3 over its JSON channel.

The socket boilerplate is the same every time - connect, send one line, read one
line - and writing it inline each session is how the port number and the framing
get remembered wrong. The channel is single-threaded inside the game's tick, so
this sends one command per invocation and does not retry: a wedged call is
better seen than papered over.
"""
import argparse
import json
import socket
import sys
import time

PORT = 5556


def call(command, timeout=10.0, **fields):
    payload = {"cmd": command}
    payload.update(fields)
    connection = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
    try:
        connection.sendall((json.dumps(payload) + "\n").encode())
        connection.settimeout(timeout)
        buffer = b""
        while b"\n" not in buffer:
            chunk = connection.recv(65536)
            if not chunk:
                break
            buffer += chunk
        return json.loads(buffer.decode().strip() or "{}")
    finally:
        connection.close()


def wait_for_world(timeout, quiet):
    """Poll until a world is loaded. new_game takes about 160 seconds."""
    started = time.time()
    while time.time() - started < timeout:
        time.sleep(6)
        try:
            state = call("ping", timeout=8)
        except Exception:
            if not quiet:
                print("%4.0fs  no reply" % (time.time() - started), flush=True)
            continue
        if not quiet:
            print("%4.0fs  %s" % (time.time() - started, state.get("state")), flush=True)
        if state.get("game_running"):
            return state
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", help="ping, list_saves, save_game, new_game, combat_state, "
                                        "attack_speed, or 'wait' to poll until a world is loaded")
    parser.add_argument("--name", help="save name, for save_game")
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if args.command == "wait":
        state = wait_for_world(args.timeout, args.quiet)
        if state is None:
            print("still not in a world after %.0f s" % args.timeout, file=sys.stderr)
            return 1
        print(json.dumps(state, ensure_ascii=False))
        return 0

    fields = {}
    if args.name is not None:
        fields["name"] = args.name
    try:
        print(json.dumps(call(args.command, timeout=min(args.timeout, 30.0), **fields),
                         ensure_ascii=False, indent=1))
    except (socket.timeout, ConnectionRefusedError, OSError) as failure:
        # Refused means no game or no script DLL; a timeout usually means the
        # game is loading, or that a previous call wedged the channel.
        print("no answer on port %d: %s" % (PORT, type(failure).__name__), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
