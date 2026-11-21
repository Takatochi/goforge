#!/usr/bin/env python3
"""
Generates single-header distributions for goforge:
    single_include/goforge/goforge.hpp  (DI, Lifecycle, BlockingPool, App, HTTP types, Gin Router, Middleware, Observability)

Note: Standalone Asio networking headers (asio_bridge.hpp / http_server.hpp) remain opt-in via GOFORGE_HAS_ASIO=1
in modular builds; the amalgamated single header works standalone with single_include/gorun/gorun.hpp on any
C++20 compiler (including Compiler Explorer / online playgrounds!).

Usage:
    python scripts/amalgamate.py
"""

import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
INCLUDE_DIR = ROOT / "include"
OUTPUT = ROOT / "single_include" / "goforge" / "goforge.hpp"
LOCAL_INCLUDE = re.compile(r'^\s*#\s*include\s*"(goforge/[^"]+)"\s*$')

_seen: set[str] = set()


def expand(rel_path: str) -> str:
    if rel_path in _seen:
        return ""
    _seen.add(rel_path)

    text = (INCLUDE_DIR / rel_path).read_text(encoding="utf-8")
    out = [f"// ---- begin {rel_path} ----"]
    skip_optional_block = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped == "#pragma once":
            continue
        if stripped.startswith("#if defined(GOFORGE_HAS_SQLITE)"):
            skip_optional_block = True
            continue
        if skip_optional_block:
            if stripped == "#endif":
                skip_optional_block = False
            continue
        match = LOCAL_INCLUDE.match(line)
        if match:
            out.append(expand(match.group(1)))
            continue
        out.append(line)
    out.append(f"// ---- end {rel_path} ----")
    return "\n".join(out)


HEADER = """#pragma once
// =====================================================================================
//  goforge — single-header core distribution (GENERATED FILE, DO NOT EDIT)
//  Fx-Style DI, Lifecycle, BlockingPool, Gin-Style Router, HttpContext, Middleware &
//  Production Observability for C++20/23 on top of gorun.
//
//  Source:  https://github.com/Takatochi/goforge
//  Author:  Artur Havor
//  License: Apache License 2.0
//
//  Regenerate with: python scripts/amalgamate.py
// =====================================================================================
"""


def main() -> None:
    body = expand("goforge/goforge.hpp")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(HEADER + body + "\n", encoding="utf-8", newline="\n")
    print(f"Wrote {OUTPUT.relative_to(ROOT)} ({len(_seen)} headers inlined)")


if __name__ == "__main__":
    main()
