#!/usr/bin/env python3
"""Build single_include/keel/keel.hpp from the headers in include/keel/.

Starting from include/keel/keel.hpp, every `#include "keel/<name>.hpp"` is
replaced by that header's contents, each header exactly once, in dependency
order. Everything else, standard includes and the headers' own include
guards, is kept as is, so the single header and the regular headers can even
be mixed in one project.

    python tools/amalgamate.py            # regenerate
    python tools/amalgamate.py --check    # fail if the committed file is stale
"""

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE_DIR = ROOT / "include"
ENTRY = SOURCE_DIR / "keel" / "keel.hpp"
OUTPUT = ROOT / "single_include" / "keel" / "keel.hpp"

INTERNAL_INCLUDE = re.compile(r'^\s*#\s*include\s+"(keel/[A-Za-z0-9_]+\.hpp)"\s*$')
VERSION = re.compile(r"#define KEEL_VERSION_(MAJOR|MINOR|PATCH) (\d+)")


def inline(path, seen, stack):
    """Return the text of `path` with internal includes expanded."""
    if path in stack:
        cycle = " -> ".join(p.name for p in stack + [path])
        sys.exit(f"amalgamate: include cycle: {cycle}")
    stack.append(path)
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = INTERNAL_INCLUDE.match(line)
        if not match:
            out.append(line)
            continue
        target = SOURCE_DIR / match.group(1)
        if not target.is_file():
            sys.exit(f"amalgamate: {path.name} includes missing {match.group(1)}")
        if target in seen:
            continue  # already inlined earlier; its include guard covers it
        seen.add(target)
        out.append(f"// ---- begin {match.group(1)} " + "-" * max(0, 60 - len(match.group(1))))
        out.append(inline(target, seen, stack))
        out.append(f"// ---- end {match.group(1)} " + "-" * max(0, 62 - len(match.group(1))))
    stack.pop()
    return "\n".join(out)


def version():
    common = (SOURCE_DIR / "keel" / "common.hpp").read_text(encoding="utf-8")
    parts = dict(VERSION.findall(common))
    return f"{parts['MAJOR']}.{parts['MINOR']}.{parts['PATCH']}"


def build():
    body = inline(ENTRY, {ENTRY}, [])
    banner = (
        f"// keel {version()}, single-header edition.\n"
        "//\n"
        "// Resilience primitives for backend and database calls in modern C++.\n"
        "// Copy this one file into your project and #include it.\n"
        "//\n"
        "// GENERATED FILE, do not edit. It is built from include/keel/ by\n"
        "// tools/amalgamate.py; change the headers there and regenerate.\n"
        "//\n"
        "// Copyright (c) 2026 the keel authors.\n"
        "// SPDX-License-Identifier: MIT\n"
    )
    return banner + "\n" + body.rstrip("\n") + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="exit non-zero if the committed single header is out of date")
    args = parser.parse_args()

    text = build()
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.is_file() else ""
        if current != text:
            sys.exit("amalgamate: single_include/keel/keel.hpp is out of date; "
                     "run `python tools/amalgamate.py` and commit the result")
        print("amalgamate: single header is up to date")
        return

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    # newline="\n" keeps LF endings on every OS, so --check is stable.
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"amalgamate: wrote {OUTPUT.relative_to(ROOT)} ({len(text.splitlines())} lines)")


if __name__ == "__main__":
    main()
