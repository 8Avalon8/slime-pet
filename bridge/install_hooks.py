#!/usr/bin/env python3
"""Register (or remove) slime_hook.py as a user-level Claude Code hook.

    python3 install_hooks.py            # install / refresh
    python3 install_hooks.py --uninstall

Merges into ~/.claude/settings.json without touching other settings or other hooks,
after writing a timestamped backup next to it. Hooks run async (no latency for Claude)
with a 5 s timeout. Entries are recognised by the script path, so re-running is idempotent.
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HOOK = os.path.join(HERE, "slime_hook.py")
SETTINGS = os.path.expanduser("~/.claude/settings.json")
# /usr/bin/python3 is always present on macOS; elsewhere use the Python running this script.
# The hook is stdlib-only (3.9+).
PYTHON = "/usr/bin/python3" if sys.platform == "darwin" else sys.executable
TOOL_EVENTS = ["PreToolUse", "PostToolUse", "PostToolUseFailure", "PermissionRequest", "PermissionDenied"]
OTHER_EVENTS = ["SessionStart", "UserPromptSubmit", "Notification", "Stop", "StopFailure", "PreCompact", "SessionEnd",
                "SubagentStart", "SubagentStop"]


def is_ours(group):
    return any("slime_hook.py" in h.get("command", "") for h in group.get("hooks", []))


def main():
    uninstall = "--uninstall" in sys.argv
    # Claude Code runs hooks through Git Bash on Windows: forward slashes survive both shells.
    python, hook = (PYTHON.replace("\\", "/"), HOOK.replace("\\", "/")) if os.name == "nt" else (PYTHON, HOOK)
    with open(SETTINGS, encoding="utf-8") as f:
        settings = json.load(f)
    backup = "%s.bak-slime-%s" % (SETTINGS, time.strftime("%Y%m%d-%H%M%S"))
    shutil.copy2(SETTINGS, backup)

    hooks = settings.setdefault("hooks", {})
    for event in TOOL_EVENTS + OTHER_EVENTS:
        groups = [g for g in hooks.get(event, []) if not is_ours(g)]
        if not uninstall:
            group = {"hooks": [{"type": "command", "command": '"%s" "%s"' % (python, hook), "timeout": 5, "async": True}]}
            if event in TOOL_EVENTS:
                group = {"matcher": "*", **group}
            groups.append(group)
        if groups:
            hooks[event] = groups
        else:
            hooks.pop(event, None)
    if not hooks:
        settings.pop("hooks")

    tmp = SETTINGS + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(settings, f, indent=2, ensure_ascii=False)
        f.write("\n")
    shutil.copymode(SETTINGS, tmp)
    os.replace(tmp, SETTINGS)
    n = len(TOOL_EVENTS + OTHER_EVENTS)
    print("%s %d slime hook events in %s (backup: %s)" % ("removed" if uninstall else "installed", n, SETTINGS, backup))


if __name__ == "__main__":
    main()
