#!/usr/bin/env python3
"""Add missing semicolons after `use` statements in .skald files.

The Skald parser (Rust-style) requires `;` after `use ...`. Many of the
test scripts omit this. This script scans for `use ...` lines that don't
end with `;` (accounting for use-trees that span multiple lines via braces)
and appends `;`.
"""
import os
import re
import sys

def fix_use_statements(src: str) -> str:
    lines = src.split('\n')
    out = []
    i = 0
    while i < len(lines):
        line = lines[i]
        # Match `use ...` that doesn't end with `;` (allowing trailing comment).
        stripped = line.rstrip()
        if re.match(r'^\s*use\b', stripped):
            # If line ends with `{`, find matching `}`.
            if stripped.endswith('{'):
                out.append(line)
                depth = 1
                i += 1
                while i < len(lines) and depth > 0:
                    l = lines[i]
                    depth += l.count('{') - l.count('}')
                    out.append(l)
                    i += 1
                # Now ensure the last appended line ends with `;`.
                # If not, append a `;` line.
                if out[-1].rstrip().endswith('}'):
                    out[-1] = out[-1].rstrip() + ';'
                else:
                    out[-1] = out[-1].rstrip() + ';'
                continue
            # Check if the line already ends with `;` (or `;` followed by comment).
            m = re.match(r'^(.*?);(\s*//.*)?$', stripped)
            if m:
                out.append(line)
            else:
                # Append `;` to the line.
                # Strip trailing whitespace and any inline comment we want to preserve.
                out.append(stripped + ';')
            i += 1
            continue
        out.append(line)
        i += 1
    return '\n'.join(out)

def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'test_scripts'
    for fname in sorted(os.listdir(root)):
        if not fname.endswith('.skald'):
            continue
        path = os.path.join(root, fname)
        with open(path) as f:
            src = f.read()
        new_src = fix_use_statements(src)
        if new_src != src:
            with open(path, 'w') as f:
                f.write(new_src)
            print(f"fixed: {fname}")

if __name__ == '__main__':
    main()
