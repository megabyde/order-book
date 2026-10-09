#!/usr/bin/env python3
"""Check that an installed Conan settings_user.yml covers this project's sanitizer values.

Usage: check_settings_subset.py <required.yml> <installed.yml>

Exits 0 when the installed file lists every required sanitizer value for every required compiler.
The installed file may contain extra compilers, sanitizer values, or unrelated subsettings such as
version:.

Parsing is limited to Conan's documented two-level settings_user.yml structure. The script uses the
standard library because PyYAML is not available in every environment where it runs. Unrecognized
input fails the subset check and requires a manual merge.
"""

import re
import sys
from pathlib import Path

COMPILER_RE = re.compile(r"^ {2,4}([\w.+-]+):$")
SANITIZER_RE = re.compile(r"^\s+sanitizer:\s*\[(.*)\]$")


def sanitizer_values(text: str) -> dict[str, set[str]]:
    """Map each compiler to the set of sanitizer values it lists."""
    values: dict[str, set[str]] = {}
    compiler = None
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].rstrip()
        if not line:
            continue
        if match := COMPILER_RE.match(line):
            compiler = match.group(1)
        elif (match := SANITIZER_RE.match(line)) and compiler:
            values.setdefault(compiler, set()).update(
                value.strip() for value in match.group(1).split(",") if value.strip()
            )
    return values


def main() -> int:
    required_path, installed_path = sys.argv[1], sys.argv[2]
    required = sanitizer_values(Path(required_path).read_text())
    installed = sanitizer_values(Path(installed_path).read_text())
    if not required:
        # If the required file becomes unparseable, an empty result would make every installed
        # configuration pass. Fail closed because Prettier controls the file layout and can change
        # it independently of this script.
        print(
            f"{required_path}: parsed no sanitizer values; its layout no longer matches what"
            " this script recognizes, so the subset check cannot run",
            file=sys.stderr,
        )
        return 1
    status = 0
    for compiler, needed in sorted(required.items()):
        missing = needed - installed.get(compiler, set())
        if missing:
            print(
                f"{installed_path}: compiler '{compiler}' lacks sanitizer"
                f" values: {', '.join(sorted(missing))}",
                file=sys.stderr,
            )
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
