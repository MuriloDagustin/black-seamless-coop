"""Nickname checks using eight disposable, hidden, muted control copies.

Replaces nicknames/settings and loads a mission. Never use a personal session.
"""
import json
import time
from pathlib import Path

from coop_smoke import Copy, wait_for
from coop_join_smoke import ready


def main():
    copies = [Copy(p) for p in range(45503, 45511)]
    names = ["Murilo", "João", "Ana Maria", "Cláudio", "Zé", "Nina_6", "100% Coop", "Bia-8"]
    for c, name in zip(copies, names):
        assert c.command("nick " + name) == name
    copies[0].command("coop host 45439")
    for c in copies[1:]:
        c.command("coop join 127.0.0.1 45439")
    wait_for(lambda: all("players=8/8" in c.command("players") for c in copies), timeout=15)

    def everyone_knows_names():
        return all(all(f'nick="{name}"' in c.command("players") for name in names) for c in copies)

    wait_for(everyone_knows_names, timeout=10)
    report = {"all_eight_names_including_host_and_self": "passed"}
    names[3] = "Amigo Renomeado"
    copies[3].command("nick " + names[3])
    names[0] = "Host João"
    copies[0].command("nick " + names[0])
    wait_for(everyone_knows_names, timeout=10)
    report["live_host_and_client_rename_reaches_everyone"] = "passed"

    # Same runtime shares black.toml in this multi-process test; the final
    # explicit setting must survive a later executable restart.
    copies[0].command("nick " + names[0])
    settings = Path("out/multi-test-runtime/black.toml").read_text(encoding="utf-8")
    assert "coop_nickname" in settings and names[0] in settings
    report["nickname_written_to_settings"] = "passed"
    assert copies[7].command("nick " + "A" * 40) == "A" * 20
    assert copies[7].command("nick    ") == "Player"
    assert copies[7].command("nick Bia-8") == "Bia-8"
    report["length_limit_and_empty_fallback"] = "passed"
    for c in copies[2:]:
        c.command("coop stop")
    copies[0].command("mission 0 2")
    wait_for(lambda: ready(copies[0], 0, 2) and ready(copies[1], 0, 2, True), timeout=45)
    for c in copies[:2]:
        c.command("pad a")
        c.command("pokef @player+2F0 1000000")
    wait_for(lambda: names[1] in copies[0].command("markers") and names[0] in copies[1].command("markers"), timeout=15)
    names[1] = "Parceiro José"
    copies[1].command("nick " + names[1])
    wait_for(lambda: names[1] in copies[0].command("markers"), timeout=10)
    report["native_in_game_markers_use_names_and_update_live"] = "passed"
    copies[0].command("nick " + names[0])
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
