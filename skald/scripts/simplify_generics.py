#!/usr/bin/env python3
"""Simplify `arr<T>::method(...)` to `arr::method(...)` in test scripts.

The Skald parser doesn't yet support type-qualified associated function calls
(e.g. `arr<v3>::with_capacity(10)`). These require backtracking on `<` to
disambiguate from comparison. Replace with `arr::method(...)` and rely on
type inference or explicit `let x: arr<T> = ...` annotations.
"""
import os
import re
import sys

def fix(src: str) -> str:
    # Pattern: `ident<...>::method(` → `ident::method(`
    # We only do this for the `arr`/`map`/`set` containers in our test scripts.
    # The generic args are simple (no nested `>`).
    src = re.sub(r'\b(arr|map|set)<(\w+)>::', r'\1::', src)
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
