#!/usr/bin/env python3
"""Final test script cleanup — fix common syntactic issues that don't match
the Skald spec:
1. `pub class X : A, implements(T)` — Skald uses `impl Trait for Type` blocks
   outside the class, not `implements(T)` in the class header. Remove the
   `implements(T)` from the class header.
2. `pub fn x() -> T, override` — `override` after `,` in a method should be
   just `override` without the leading comma (handled by parser).
3. Some files use `extern fn` without the `"C"` abi. That's fine — parser
   handles it. But ensure `;` after.
"""
import os
import re
import sys

def fix(src: str) -> str:
    # Remove `, implements(TraitName)` from class headers.
    src = re.sub(r',\s*implements\(\s*\w+\s*\)', '', src)
    # Remove `: implements(TraitName)` (no parent class).
    src = re.sub(r':\s*implements\(\s*\w+\s*\)', '', src)
    return src

def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'test_scripts'
    for fname in sorted(os.listdir(root)):
        if not fname.endswith('.skald'):
            continue
        path = os.path.join(root, fname)
        with open(path) as f:
            src = f.read()
        new_src = fix(src)
        if new_src != src:
            with open(path, 'w') as f:
                f.write(new_src)
            print(f"fixed: {fname}")

if __name__ == '__main__':
    main()
