"""Formation reference regression on disposable --coop-control copies.

The missing-anchor case crashes the unpatched native helper at 0x1A0086,
matching Windows fault offsets in releases 38185ce and 6f7d9bf.
"""
import argparse
import json
import math
import re
import time

from coop_smoke import Copy, wait_for


def position(reply):
    return tuple(map(float, re.search(r"position=([^ ]+)", reply)[1].split(",")))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--ports", type=int, nargs="+", default=[45703, 45704, 45705])
    args = p.parse_args()
    copies = [Copy(port) for port in args.ports]
    report = []
    for c in copies:
        good = c.command("formationprobe valid")
        missing = c.command("formationprobe missing-anchor")
        assert position(good) == position(missing), (good, missing)
        assert "anchor=00000000" in missing, "The helper must not rewrite the AI's links"
        orphan = c.command("formationprobe missing-group")
        assert all(math.isfinite(v) for v in position(orphan)), orphan
        assert "group=00000000" in orphan
        before = c.command("ping")
        wait_for(lambda: c.command("ping") != before)
        # Normal calls must still match after repeated exceptional records.
        for _ in range(8):
            c.command("formationprobe missing-anchor")
            c.command("formationprobe missing-group")
        assert all(math.isfinite(v) for v in position(c.command("formationprobe valid")))
        report.append({"port": c.port, "missing_anchor_matches_live_leader": True,
                       "missing_group_holds_follower": orphan, "simulation_continues": True})
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
