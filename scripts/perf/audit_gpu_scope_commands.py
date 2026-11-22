#!/usr/bin/env python3
"""Compare non-timer Vulkan command tokens before/after a timer-only conversion."""
import argparse
import difflib
import json
from pathlib import Path
import re
import subprocess


def commands(source):
    # Preserve string literals while discarding comments (including apparent calls in comments).
    source = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
                    lambda m: " " if m[0].startswith(("//", "/*")) else m[0], source, flags=re.S)
    result = []
    ignored = {"vkCmdWriteTimestamp2", "vkCmdResetQueryPool"}
    for match in re.finditer(r'\b(vkCmd\w+)\s*\(', source):
        if match[1] in ignored:
            continue
        depth, end = 1, match.end()
        while depth and end < len(source):
            if source[end] == "(":
                depth += 1
            elif source[end] == ")":
                depth -= 1
            end += 1
        result.append(re.sub(r'\s+', '', source[match.start():end]))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    args = parser.parse_args()
    paths = subprocess.check_output(["git", "diff", "--name-only", args.base, "--", "indra/newview/vkgfx"],
                                    cwd=args.root, text=True).splitlines()
    results = []
    for path in paths:
        if not path.endswith(".cpp"):
            continue
        old = subprocess.check_output(["git", "show", args.base + ":" + path], cwd=args.root, text=True)
        before, after = commands(old), commands((args.root / path).read_text())
        results.append({"path": path, "before": len(before), "after": len(after), "identical": before == after,
                        "difference": list(difflib.unified_diff(before, after))[:80] if before != after else []})
    print(json.dumps(results, indent=2))
    return 0 if all(row["identical"] for row in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
