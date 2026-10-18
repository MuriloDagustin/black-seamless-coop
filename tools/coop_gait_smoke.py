"""Require real leg animation, not merely a changed clip name, on disposable copies.

Loads missions and moves both players. Keep copies hidden and muted.
"""
import argparse
import json
import re
import struct
import time

from coop_smoke import Copy, wait_for
from coop_join_smoke import ready, state
from coop_jitter_smoke import Bursts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ports", type=int, nargs=2, default=[45503, 45504])
    parser.add_argument("--server-port", type=int, default=45439)
    parser.add_argument("--proxy-port", type=int, default=45440)
    parser.add_argument("--levels", type=int, nargs="+", default=[0, 1])
    args = parser.parse_args()
    copies = [Copy(p) for p in args.ports]
    if all(state(c)["load"] == "0" for c in copies):
        wait_for(lambda: all(c.command("page") == "PressStart" for c in copies), timeout=25)
    for c in copies:
        c.command("coop stop")
    time.sleep(.3)
    report = []
    with Bursts(args.proxy_port, args.server_port):
        copies[0].command(f"coop host {args.server_port}")
        copies[1].command(f"coop join 127.0.0.1 {args.proxy_port}")
        for level in args.levels:
            old_load = state(copies[0])["load"]
            copies[0].command(f"mission {level} 0")
            wait_for(lambda: state(copies[0])["load"] != old_load and
                     all(ready(c, level, 0, i > 0) for i, c in enumerate(copies)), timeout=45)
            time.sleep(1)
            for c in copies:
                c.command("pad a")
                c.command("pokef @player+2F0 1000000")
            time.sleep(.5)
            starts = [state(c)["feet"].replace(",", " ") for c in copies]
            if level == 0:
                starts = ["-84 -3.6 40", "-80 -3.6 40"]
            for mover_index in (0, 1):
                mover, observer = copies[mover_index], copies[1 - mover_index]
                body = int(re.search(r"body=([0-9A-F]+)", observer.command("state"))[1], 16)
                assert body, observer.command("players")
                animation = observer.read(body + 0x328)[0]
                controller = observer.read(body + 0x324)[0]
                model_type = observer.read(body + 0x320)[0]
                skeleton = observer.read(animation + 0x54)[0]
                model = observer.read(animation + 0x50)[0]
                mapping = observer.read(model + 0x34)[0]
                leg_ids = observer.read(model_type + 0x28, 4)  # native knee/ankle IDs
                leg_bones = [observer.read(mapping + i * 4)[0] for i in leg_ids]

                def clip():
                    return struct.pack("<4I", *observer.read(animation + 0x5C, 4))[:13].split(b"\0")[0].decode()

                def pose():
                    matrices = observer.read(skeleton + 0x50)[0]
                    values = []
                    for bone in leg_bones:
                        values += list(map(float, observer.command(f"peekf {matrices + bone * 64:X} 12").split()))
                    return values

                for gait, stick in (("walk", .45), ("run", 1)):
                    positions = ["-80 -3.6 40", "-84 -3.6 40"] if level == 0 else ["-120.1 26.5 -66", "-121.5 26.8 -63.9"]
                    for c, start in zip((mover, observer), positions):
                        c.command("pad a")
                        c.command("teleport " + start)
                        c.command("yaw " + ("0" if level == 0 else "68.4"))
                    time.sleep(.7)
                    idle = clip()
                    start = tuple(map(float, state(mover)["feet"].split(",")))
                    for attempt in range(8):
                        mover.command(f"stick 0 {stick} 0 0 2500")
                        time.sleep(.3)
                        feet = tuple(map(float, state(mover)["feet"].split(",")))
                        if sum((a - b) ** 2 for a, b in zip(start, feet)) > .04:
                            break
                        for c in copies:
                            c.command("pad a") # tutorials can block input while play=1
                        time.sleep(.3)
                    poses, clips = [], []
                    until = time.monotonic() + 1
                    while time.monotonic() < until:
                        clips.append(clip())
                        poses.append(pose())
                        time.sleep(.03)
                    mover.command("release")
                    end = tuple(map(float, state(mover)["feet"].split(",")))
                    distance = sum((a - b) ** 2 for a, b in zip(start, end)) ** .5
                    assert distance > .5, (level, mover_index, gait, start, end)
                    assert any(s != idle and not s.endswith("_MVI") for s in clips), (level, mover_index, gait, clips)
                    swing = max(max(p[i] for p in poses) - min(p[i] for p in poses) for i in range(len(poses[0])))
                    assert swing > .15, ("Legs stayed still", level, mover_index, gait, swing, clips)
                    assert float(observer.command(f"peekf {controller + 0x20:X} 1")) == 0, "Animation must not add local physics movement"
                    for c in copies:
                        c.command("pad a")
                    wait_for(lambda: clip() == idle, timeout=4)
                    report.append({"level": level, "mover": "host" if mover_index == 0 else "client",
                                   "gait": gait, "clips": sorted(set(clips)), "leg_rotation_swing": round(swing, 3)})
        print(json.dumps({"state_burst_ms": 180, "real_leg_animation": report}, indent=2))
    for c in copies:
        c.command("coop stop")


if __name__ == "__main__":
    main()
