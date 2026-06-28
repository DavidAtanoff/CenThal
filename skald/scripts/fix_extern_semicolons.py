#!/usr/bin/env python3
"""Add missing semicolons after `extern fn ...` declarations.

extern fn declarations require a trailing `;` (like Rust). Several test
scripts omit this. This script scans for `extern "..." fn ...)` lines
that don't end with `;` and adds one.
"""
import os
import re
import sys

def fix_extern(src: str) -> str:
    lines = src.split('\n')
    out = []
    for line in lines:
        stripped = line.rstrip()
        # Match `extern "C" fn name(...) -> Type` or `extern "C" fn name(...)`
        # that doesn't end with `;` or `{`.
        if re.match(r'^\s*extern\b', stripped):
            # Walk forward to find the end of the declaration.
            # For now, just check the current line — extern decls in our tests
            # are single-line.
            if not stripped.endswith(';') and not stripped.endswith('{'):
                # Heuristic: if it ends with `)` or a type, add `;`.
                if stripped.endswith(')') or re.search(r'->\s*\w+$', stripped):
                    out.append(stripped + ';')
                    continue
        out.append(line)
    return '\n'.join(out)

def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'test_scripts'
    for fname in sorted(os.listdir(root)):
        if not fname.endswith('.skald'):
            continue
        path = os.path.join(root, fname)
        with open(path) as f:
            src = f.read()
        new_src = fix_extern(src)
        if new_src != src:
            with open(path, 'w') as f:
                f.write(new_src)
            print(f"fixed: {fname}")

if __name__ == '__main__':
    main()
