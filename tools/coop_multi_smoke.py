"""Eight-player checks on disposable hidden, muted --coop-control copies.

Start ports 45503..45510, host the first on UDP 45439 and join the others.
Load a fresh first mission on the host before running this test. It collects
items, fires weapons, disconnects/rejoins the last player and ends the mission.
"""
import argparse
import hashlib
import json
import math
import re
import socket
import struct
import time
from pathlib import Path

from coop_smoke import Copy, wait_for
from coop_join_smoke import ready, removed, state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ports", type=int, nargs="+", default=list(range(45503, 45511)))
    parser.add_argument("--server-port", type=int, default=45439)
    args = parser.parse_args()
    copies = [Copy(p) for p in args.ports]
    assert len(copies) == 8
    report = {}

    def hints():
        for c in copies:
            c.command("pad a")  # native tutorial overlays can leave play=1
        time.sleep(.6)

    def all_count(key, value):
        return all(c.counts()[key] == value for c in copies)

    def shots(c):
        return tuple(map(int, state(c)["shots"].split("/")))

    wait_for(lambda: all(ready(c, 0, 2, i > 0) for i, c in enumerate(copies)), timeout=60)
    wait_for(lambda: all(len(re.findall(r"body=(?!00000000)[0-9A-F]+", c.command("players"))) == 7 for c in copies), timeout=15)
    for c in copies:
        assert "players=8/8" in c.command("players")
        bodies = re.findall(r"body=([0-9A-F]+)", c.command("players"))
        assert len(set(bodies)) == 7
        assert not any(c.counts().values()), "Use a fresh mission"
        c.command("pokef @player+2F0 1000000")
    report["eight_players_auto_join_and_seven_distinct_bodies_each"] = "passed"
    hints()

    # Move away from the shared spawn with genuine controller input.
    mover = copies[1]
    start = tuple(map(float, state(mover)["feet"].split(",")))
    mover.command("yaw 0")
    mover.command("stick 0 1 0 0 1000")
    time.sleep(.6)
    mover.command("release")
    end = tuple(map(float, state(mover)["feet"].split(",")))
    assert math.dist(start, end) > .3, (start, end)
    report["movement_from_shared_spawn"] = "passed"

    item = 0x72AE2D2C5039ACEC
    pickup = copies[5].pickup(item)
    position = copies[5].command(f"peekf {pickup + 0xA0:X} 3")
    copies[5].command("teleport " + position)
    wait_for(lambda: all_count("intel", 1), timeout=10)
    wait_for(lambda: all(removed(c, item) for c in copies), timeout=10)
    report["client_collection_reaches_everyone_and_removes_item"] = "passed"

    actions = {}
    for line in copies[0].command("objectives").splitlines():
        match = re.match(r"([0-9A-F]+) flags=(\d+)", line)
        if match:
            actions.setdefault(int(match[2]), []).append(match[1])
    for c in copies:
        c.socket.send(("objective " + actions[4][0]).encode())
    for c in copies:
        assert c.socket.recv(8000).decode().startswith("asked")
    wait_for(lambda: all_count("blackmail", 1), timeout=10)
    copies[3].command("objective " + actions[4][1])
    wait_for(lambda: all_count("blackmail", 2), timeout=10)
    for index, flag, key in ((4, 16, "recon"), (6, 32, "armament")):
        copies[index].command("objective " + actions[flag][0])
        wait_for(lambda: all_count(key, 1), timeout=10)
        hints()
    report["eight_simultaneous_collections_deduplicate_all_four_categories"] = "passed"

    hints()
    # Separate the solid partner bodies; a muzzle inside another character is
    # obstructed by the native weapon code before a bullet can be spawned.
    for i, c in enumerate(copies):
        c.command(f"teleport {-84 + i * 3} -3.6 40")
    time.sleep(1)
    hints()
    before = [shots(c) for c in copies]
    for c in copies:
        c.command("pitch -55")
        c.command("pad rt 200")
    time.sleep(.6)
    for c in copies:
        c.command("release")
    after = [shots(c) for c in copies]
    sent = [a[0] - b[0] for a, b in zip(after, before)]
    assert all(n > 0 for n in sent), sent
    expected = [sum(sent) - n for n in sent]
    wait_for(lambda: all(shots(c)[1] - b[1] >= n and shots(c)[2] - b[2] >= n
                        for c, b, n in zip(copies, before, expected)), timeout=10)
    report["all_eight_shooters_reach_the_other_seven_without_ack_collision"] = "passed"

    # A ninth endpoint receives an explicit full response without evicting anyone.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as extra:
        extra.settimeout(3)
        sha = hashlib.sha1(Path("assets/default.xbe").read_bytes()).hexdigest().encode()
        hello = struct.pack("<IHBBIII", 0x4F434B42, 17, 1, 15, 0xABCDE123, 1, 0) + sha
        extra.sendto(hello, ("127.0.0.1", args.server_port))
        reply = extra.recv(1500)
        assert struct.unpack_from("<IHBB", reply) == (0x4F434B42, 17, 12, 0xF0)
    assert all("players=8/8" in c.command("players") for c in copies)
    report["ninth_connection_rejected_without_displacing_players"] = "passed"

    last = copies[-1]
    last.command("coop stop")
    wait_for(lambda: all("players=7/8" in c.command("players") for c in copies[:-1]), timeout=10)
    # Reload just the disconnected client: its new join must restore all progress.
    last.command("mission 0 2")
    wait_for(lambda: ready(last, 0, 2), timeout=35)
    assert not any(last.counts().values())
    last.command(f"coop join 127.0.0.1 {args.server_port}")
    wait_for(lambda: ready(last, 0, 2, True) and last.counts() == copies[0].counts(), timeout=45)
    wait_for(lambda: all("players=8/8" in c.command("players") for c in copies), timeout=10)
    assert removed(last, item)
    report["leave_and_rejoin_restores_shared_objectives"] = "passed"

    copies[0].command("flow complete")
    def complete(c):
        game = c.read(0x2D45F0)[0]
        return c.read(game + 0x20F80)[0] == game + 0x2104C
    deadline = time.monotonic() + 15
    while not all(complete(c) for c in copies):
        assert time.monotonic() < deadline, "Not every copy completed"
        for c in copies:
            if not complete(c):
                c.command("pad a")
        time.sleep(.5)
    assert all(c.counts() == copies[0].counts() for c in copies)
    report["all_eight_complete_with_equal_objectives"] = "passed"
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
