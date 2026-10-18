"""Auto-join integration checks using two disposable hidden, muted test copies.

Start both copies with --coop-control 45503/45504 (no --coop), then run:
  python tools/coop_join_smoke.py --ports 45503 45504
All game interaction uses the loopback UDP control channel.
"""
import argparse
import json
import math
import re
import time

from coop_smoke import Copy, wait_for


def state(copy):
    return dict(re.findall(r'(\w+)=("[^"]*"|\S+)', copy.command("state")))


def ready(copy, level, difficulty, joined=False):
    s = state(copy)
    game = copy.read(0x2D45F0)[0]
    return (s["level"] == str(level) and s["difficulty"] == str(difficulty)
            and s["play"] == "1" and (not joined or s["joining"] == "0")
            and copy.read(game + 0x20F7C)[0] == 0x1C
            and copy.read(game + 0x21084)[0] == 0)


def near(host, join):
    a, b = (tuple(map(float, state(c)["feet"].split(","))) for c in (host, join))
    assert math.dist(a, b) < 8, (a, b)


def removed(copy, name):
    try:
        return not copy.active(copy.pickup(name))
    except RuntimeError as error:
        if "not loaded" in str(error):
            return True  # removal releases the record and clears its name
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ports", type=int, nargs=2, default=[45503, 45504])
    parser.add_argument("--server-port", type=int, default=45420)
    args = parser.parse_args()
    host, join = (Copy(p) for p in args.ports)
    report = {}
    assert state(host)["load"] == state(join)["load"] == "0", "Use fresh test copies"
    wait_for(lambda: all(c.command("page") == "PressStart" for c in (host, join)), timeout=20)
    for c in (host, join):
        assert "running=0" in c.command("coop"), "Opening the game must not reconnect a saved co-op role"
        assert state(c)["joining"] == "0"
    report["startup_does_not_reconnect_saved_session"] = "passed"
    host.command(f"coop host {args.server_port}")
    join.command(f"coop join 127.0.0.1 {args.server_port}")
    wait_for(lambda: "WAITING HOST MISSION" in join.command("state"))
    assert state(join)["load"] == "0", "Do not load a mission while host is in menus"
    host.command("mission 0 2")
    wait_for(lambda: ready(join, 0, 2, True), timeout=40)
    near(host, join)
    first_load = state(join)["load"]
    time.sleep(1)
    assert state(join)["load"] == first_load, "Repeated state packets must not restart the mission"
    report["join_from_title_waits_then_loads_host_map_and_hard_difficulty"] = "passed"

    # Start hosting AFTER collecting an actual map item, as a real late host.
    for c in (host, join):
        c.command("coop stop")
    wait_for(lambda: all(c.command("coop").startswith("COOP CONTROL") for c in (host, join)))
    host.command("mission 0 2")
    wait_for(lambda: ready(host, 0, 2) and state(host)["load"] != first_load, timeout=40)
    host.command("pokef @player+2F0 1000000")
    item = 0x72AE2D2C5039ACEC
    pickup = host.pickup(item)
    position = host.command(f"peekf {pickup + 0xA0:X} 3")
    host.command("teleport " + position)
    wait_for(lambda: host.counts()["intel"] == 1)
    host.dismiss_hint()
    host.command("teleport -84 -3.6 40")
    host.command("pad start")
    wait_for(lambda: state(host)["play"] == "0")
    host.command(f"coop host {args.server_port}")
    previous = state(join)["load"]
    join.command(f"coop join 127.0.0.1 {args.server_port}")
    wait_for(lambda: ready(join, 0, 2, True) and state(join)["load"] != previous, timeout=40)
    near(host, join)
    assert host.counts() == join.counts(), (host.counts(), join.counts())
    wait_for(lambda: removed(join, item))
    # Walking over the same item after the baseline must not count it again.
    join.command("teleport " + position)
    time.sleep(0.4)
    assert join.counts()["intel"] == host.counts()["intel"] == 1
    report["late_host_paused_objectives_and_pickup_removal"] = "passed"

    # Different map and difficulty: both must follow the host's new selection.
    host.command("mission 1 0")
    wait_for(lambda: ready(join, 1, 0, True) and ready(host, 1, 0), timeout=40)
    near(host, join)
    assert host.counts() == join.counts()
    report["host_changes_map_and_difficulty"] = "passed"

    # Reconnect from another mission with a different local difficulty.
    join.command("coop stop")
    wait_for(lambda: join.command("coop").startswith("COOP CONTROL"))
    join.command("mission 3 3")
    wait_for(lambda: ready(join, 3, 3), timeout=40)
    join.command(f"coop join 127.0.0.1 {args.server_port}")
    wait_for(lambda: ready(join, 1, 0, True), timeout=40)
    near(host, join)
    report["join_replaces_a_different_local_map_and_black_ops_difficulty"] = "passed"
    host.command("mission 0 3")
    wait_for(lambda: ready(join, 0, 3, True), timeout=40)
    assert host.read(int(host.command("peek 2D45D4 1"), 16) + 0x5CB0)[0] & 0xFFFF == 0x101
    assert join.read(int(join.command("peek 2D45D4 1"), 16) + 0x5CB0)[0] & 0xFFFF == 0x101
    report["host_black_ops_weapon_variant_and_unlimited_ammo"] = "passed"
    previous = state(join)["load"]
    host.command("mission 0 3")
    wait_for(lambda: ready(join, 0, 3, True) and state(join)["load"] != previous, timeout=40)
    final_load = state(join)["load"]
    time.sleep(1)
    assert state(join)["load"] == final_load
    report["same_map_restart_follows_once"] = "passed"
    for c in (host, join):
        c.command("coop stop")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
