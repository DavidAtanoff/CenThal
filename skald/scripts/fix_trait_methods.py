#!/usr/bin/env python3
"""Add semicolons to trait method declarations that lack them.

Trait method signatures require `;` (or a default body `{ ... }`):
    pub trait Foo {
        fn bar(x: i32) -> i32;        // ← needs `;`
        fn baz() { ... }              // ← default body, no `;` needed
    }
"""
import os
import re
import sys

def fix_trait_methods(src: str) -> str:
    lines = src.split('\n')
    out = []
    in_trait = False
    depth = 0
    body_depth = -1
    for i, line in enumerate(lines):
        stripped = line.strip()
        prev_depth = depth
        depth += line.count('{') - line.count('}')

        if re.search(r'\btrait\s+\w+', line) and '{' in line:
            in_trait = True
            body_depth = depth
        elif in_trait and depth < body_depth:
            in_trait = False
            body_depth = -1

        # If we're inside a trait body and the line is `fn name(...) -> Type`
        # (no `;` and no `{`), add `;`.
        if (in_trait and depth == body_depth and prev_depth == body_depth
            and re.match(r'(pub\s+|private\s+|protected\s+)?fn\s+\w+\s*\([^)]*\)(\s*->\s*[^{;]+)?$', stripped)
            and not stripped.endswith(';')
            and not stripped.endswith('{')):
            indent = line[:len(line) - len(line.lstrip())]
            out.append(f"{indent}{stripped};")
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
        new_src = fix_trait_methods(src)
        if new_src != src:
            with open(path, 'w') as f:
                f.write(new_src)
            print(f"fixed: {fname}")

if __name__ == '__main__':
    main()
