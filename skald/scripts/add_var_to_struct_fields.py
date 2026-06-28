#!/usr/bin/env python3
"""Add `var` prefix to struct fields that lack it.

Spec §5.4/§12.4 require `var` before struct field declarations:
    struct Foo {
        var x: i32
        var y: i32
    }

Many test scripts use Rust-style `struct Foo { x: i32 }` without `var`.
This script adds `var` to lines inside struct/class bodies that look like
field declarations but lack the `var`/`let` prefix.
"""
import os
import re
import sys

def fix_struct_fields(src: str) -> str:
    lines = src.split('\n')
    out = []
    depth = 0
    in_struct_or_class = False  # are we inside a struct/class body?
    body_depth = -1  # the `{` depth at which the current struct/class body opens
    for line in lines:
        stripped = line.strip()
        # Track brace depth.
        opens = line.count('{')
        closes = line.count('}')
        prev_depth = depth
        depth += opens - closes

        # Detect entering a struct/class body.
        if (re.search(r'\b(struct|class)\s+\w+', line) and '{' in line):
            in_struct_or_class = True
            body_depth = depth  # depth AFTER the open brace
        elif in_struct_or_class and depth < body_depth:
            # We've closed past the struct/class body.
            in_struct_or_class = False
            body_depth = -1

        # If we're inside the struct/class body (at body_depth exactly), and
        # the line looks like a field declaration without `var`/`let`, add it.
        if (in_struct_or_class and depth == body_depth
            and prev_depth == body_depth  # not the line that opened the brace
            and stripped
            and not stripped.startswith('//')
            and not stripped.startswith('///')
            and not stripped.startswith('pub ')
            and not stripped.startswith('private ')
            and not stripped.startswith('protected ')
            and not stripped.startswith('@')
            and not stripped.startswith('var ')
            and not stripped.startswith('let ')
            and not stripped.startswith('fn ')
            and not stripped.startswith('static ')
            and not stripped.startswith('mod ')
            and not stripped.startswith('use ')
            and not stripped.startswith('impl ')
            and not stripped.startswith('trait ')
            and not stripped.startswith('enum ')
            and not stripped.startswith('struct ')
            and not stripped.startswith('class ')
            and not stripped.startswith('const ')
            and not stripped.startswith('type ')
            and not stripped.startswith('alias ')
            and not stripped.startswith('}')):
            # Check if it looks like `name: Type` or `name: Type = init`.
            if re.match(r'^\w+\s*:', stripped):
                # Add `var ` prefix (preserving indentation).
                indent = line[:len(line) - len(line.lstrip())]
                out.append(f"{indent}var {stripped}")
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
        new_src = fix_struct_fields(src)
        if new_src != src:
            with open(path, 'w') as f:
                f.write(new_src)
            print(f"fixed: {fname}")

if __name__ == '__main__':
    main()
