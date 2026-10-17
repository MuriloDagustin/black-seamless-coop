"""Integration checks against two disposable, muted --coop-control instances.

Load the first mission on both copies with fresh objective counters, then run:
  python tools/coop_smoke.py --ports 45503 45504
The test moves players, invokes game scripts and finishes the mission. Do not
point it at a play session you want to keep.
"""
import argparse
import json
import re
import socket
import struct
import time


class Copy:
    def __init__(self, port):
        self.port = port
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.connect(("127.0.0.1", port))
        self.socket.settimeout(3)

    def command(self, text):
        self.socket.send(text.encode())
        reply = self.socket.recv(8000).decode()
        if reply.startswith("error"):
            raise RuntimeError(reply)
        return reply

    def read(self, address, count=1):
        return [int(x, 16) for x in self.command(f"peek {address:X} {count}").split()]

    def counts(self):
        stats = self.read(self.read(0x2D12CC)[0])[0]
        data = struct.pack("<2I", *self.read(stats + 0x14, 2))
        return dict(zip(("primary", "blackmail", "intel", "recon", "armament"), data[2:7]))

    def pickup(self, name):
        manager = self.read(0x2D0D64)[0]
        for i in range(64):
            address = manager + i * 0x160
            lo, hi = self.read(address + 8, 2)
            if lo | hi << 32 == name:
                return address
        raise RuntimeError(f"pickup {name:016X} not loaded")

    def active(self, address):
        return self.read(address + 0x138)[0] >> 24 != 0

    def dismiss_hint(self):
        # Recon/armament hints can stop controller input while the world still
        # reports a recent player update. Confirm them explicitly in test copies.
        self.command("pad a")
        time.sleep(0.5)


def wait_for(test, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if test():
            return
        time.sleep(0.1)
    raise AssertionError("condition did not become true")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ports", type=int, nargs=2, default=[45503, 45504])
    args = parser.parse_args()
    copies = [Copy(p) for p in args.ports]
    report = {}
    for copy in copies:
        state = copy.command("state")
        assert "level=0 unit=1 play=1" in state, state
        assert not any(copy.counts().values()), "Start with fresh first-mission objectives"
        copy.command("pokef @player+2F0 1000000")

    # A real map collectible must disappear on BOTH copies, with no extra count
    # when the other player later walks over the same position.
    item = 0x72AE2D2C5039ACEC
    pickups = [c.pickup(item) for c in copies]
    position = copies[0].command(f"peekf {pickups[0] + 0xA0:X} 3")
    copies[0].command("teleport " + position)
    wait_for(lambda: all(c.counts()["intel"] == 1 for c in copies))
    wait_for(lambda: all(not c.active(p) for c, p in zip(copies, pickups)))
    copies[1].command("teleport " + position)
    time.sleep(0.4)
    assert all(c.counts()["intel"] == 1 for c in copies)
    report["collectible_removal_and_deduplication"] = "passed"

    actions = {}
    for line in copies[0].command("objectives").splitlines():
        match = re.match(r"([0-9A-F]+) flags=(\d+)", line)
        if match:
            actions.setdefault(int(match[2]), []).append(match[1])
    # Both players fire the SAME original blackmail action concurrently. Crossing
    # events must merge as one collection, while DIFFERENT actions must add up.
    action = actions[4][0]
    for copy in copies:
        copy.socket.send(f"objective {action}".encode())
    for copy in copies:
        assert copy.socket.recv(8000).decode().startswith("asked")
    wait_for(lambda: all(c.counts()["blackmail"] == 1 for c in copies))
    copies[1].command("objective " + actions[4][1])
    wait_for(lambda: all(c.counts()["blackmail"] == 2 for c in copies))
    for flag, key in ((16, "recon"), (32, "armament")):
        copies[0].command("objective " + actions[flag][0])
        wait_for(lambda: all(c.counts()[key] == 1 for c in copies))
        for copy in copies:
            copy.dismiss_hint()
    report["four_categories_and_crossing_events"] = copies[0].counts()

    # Movement is real controller input. The body's selected clips must change
    # from standing to locomotion and then return to standing after release.
    for index, copy in enumerate(copies):
        copy.dismiss_hint()
        # Keep the players apart: the partner's solid body blocks movement when
        # the test teleports both players to exactly the same position.
        copy.command(f"teleport {-84 if index == 0 else -80} -3.6 40")
        copy.command("yaw 0")
    time.sleep(1)
    state = copies[0].command("state")
    body = int(re.search(r"body=([0-9A-F]+)", state)[1], 16)
    assert body
    anim = copies[0].read(body + 0x328)[0]

    def clips():
        return copies[0].command(f"peek {anim + 0x5C:X} 10")

    standing = clips()
    copies[1].command("stick 0 0.5 0 0 1000")
    time.sleep(0.7)
    walking = clips()
    assert walking != standing, (standing, walking)
    copies[1].command("release")
    time.sleep(1)
    copies[1].command("stick 0 1 0 0 2500")
    time.sleep(1)
    feet = re.search(r"feet=([^ ]+)", copies[1].command("state"))[1]
    assert float(feet.split(",")[2]) > 41, "Movement test was blocked"
    moving = clips()
    assert moving != standing, (standing, moving)
    copies[1].command("release")
    # Entering the courtyard can open a tutorial hint and freeze both worlds.
    # Resume simulation before asserting that an animation returns to idle.
    time.sleep(0.5)
    for copy in copies:
        copy.dismiss_hint()
    time.sleep(1.5)
    assert clips() == standing, (standing, clips())
    report["walking_running_and_idle_clip_transitions"] = "passed"

    # Test the same complete request the game's end trigger uses. Both copies'
    # in-level substate must become the COMPLETE state (game+0x2104C).
    copies[0].command("flow complete")

    def complete(copy):
        game = copy.read(0x2D45F0)[0]
        return copy.read(game + 0x20F80)[0] == game + 0x2104C

    deadline = time.monotonic() + 10
    while not all(complete(c) for c in copies):
        assert time.monotonic() < deadline, "Both copies did not complete"
        # A tutorial can still be open on the peer: let its queued world events
        # run by confirming that hint, without advancing the completed copy.
        for copy in copies:
            if not complete(copy):
                copy.command("pad a")
        time.sleep(0.6)
    assert copies[0].counts() == copies[1].counts()
    report["both_copies_complete_with_equal_objectives"] = "passed"
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
