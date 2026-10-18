"""Locomotion regression through a UDP proxy delivering states in 180 ms bursts.

Use two disposable, hidden, muted control copies. This replaces their missions.
"""
import argparse
import json
import math
import re
import select
import socket
import threading
import time

from coop_smoke import Copy, wait_for
from coop_join_smoke import ready, state


class Bursts:
    def __init__(self, port, host_port):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", port))
        self.host = ("127.0.0.1", host_port)
        self.client = None
        self.pending = {}
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def run(self):
        flush = time.monotonic() + .18
        while not self.stopped.is_set():
            if select.select([self.socket], [], [], .005)[0]:
                data, source = self.socket.recvfrom(1500)
                if source == self.host:
                    target = self.client
                else:
                    self.client = source
                    target = self.host
                if target:
                    if len(data) >= 20 and data[6] == 3:
                        self.pending[target] = data  # latest state wins
                    else:
                        self.socket.sendto(data, target)
            if time.monotonic() >= flush:
                for target, data in self.pending.items():
                    self.socket.sendto(data, target)
                self.pending.clear()
                flush = time.monotonic() + .18

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.stopped.set()
        self.thread.join(2)
        self.socket.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ports", nargs=2, type=int, default=[45503, 45504])
    parser.add_argument("--server-port", type=int, default=45439)
    parser.add_argument("--proxy-port", type=int, default=45440)
    args = parser.parse_args()
    host, client = [Copy(p) for p in args.ports]
    for c in (host, client):
        c.command("coop stop")
    time.sleep(.3)
    with Bursts(args.proxy_port, args.server_port):
        host.command(f"coop host {args.server_port}")
        client.command(f"coop join 127.0.0.1 {args.proxy_port}")
        host.command("mission 0 2")
        wait_for(lambda: ready(host, 0, 2) and ready(client, 0, 2, True), timeout=60)
        time.sleep(4)  # departed participants' native bodies have been released
        for i, c in enumerate((host, client)):
            c.command("pad a")
            c.command("pokef @player+2F0 1000000")
            c.command(f"teleport {-84 + i * 4} -3.6 40")
            c.command("yaw 0")
        time.sleep(1)
        for c in (host, client):
            c.command("pad a")
        time.sleep(1)
        body = int(re.search(r"body=([0-9A-F]+)", host.command("state"))[1], 16)
        assert body
        animation = host.read(body + 0x328)[0]
        controller = host.read(body + 0x324)[0]

        def speed():
            return float(host.command(f"peekf {controller + 0x84:X} 1"))

        def clips():
            # Current lower-body clip only. The following bytes include the
            # previous clip, which legitimately changes after walking.
            return host.command(f"peek {animation + 0x5C:X} 3")

        standing = clips()
        report = {}
        for name, stick in (("walk", .45), ("run", 1)):
            for c in (host, client):
                c.command("pad a")
            client.command("teleport -80 -3.6 40")
            time.sleep(.5)
            start = tuple(map(float, state(client)["feet"].split(",")))
            client.command(f"stick 0 {stick} 0 0 2200")
            time.sleep(.4)
            samples, animated = [], False
            until = time.monotonic() + 1
            while time.monotonic() < until:
                samples.append(speed())
                animated |= clips() != standing
                time.sleep(.03)
            client.command("release")
            end = tuple(map(float, state(client)["feet"].split(",")))
            assert math.dist(start, end) > .5, (name, start, end)
            assert animated, f"{name}: standing animation while moving"
            assert samples and sum(s > .6 for s in samples) / len(samples) >= .8, (name, samples)
            for c in (host, client):
                c.command("pad a")  # courtyard tutorial may freeze the last moving clip
            wait_for(lambda: speed() < .15, timeout=1.5)
            try:
                wait_for(lambda: clips() == standing, timeout=3)
            except AssertionError:
                raise AssertionError((name, "idle clips", standing, clips(), samples))
            report[name] = {"samples": len(samples), "min_speed": round(min(samples), 2),
                            "max_speed": round(max(samples), 2), "returned_to_idle": True}
        print(json.dumps({"state_burst_ms": 180, "locomotion": report}, indent=2))
    client.command("coop stop")
    host.command("coop stop")


if __name__ == "__main__":
    main()
