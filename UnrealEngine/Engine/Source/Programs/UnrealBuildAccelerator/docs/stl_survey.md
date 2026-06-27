# UBA STL Survey — `std::*` usage & in-house containers

Survey date: 2026-04-22
Codebase root: `D:/devel/fn/Engine/Source/Programs/UnrealBuildAccelerator/`

Primary scope  : `Core/{Public,Private}/`, `Detours/{Public,Private,Private/Windows,Private/PosixOS}/`
Secondary scope: `Common/`, `Cli/`, `Ninja/`, `Host/`, `Agent/`, `Visualizer/`

The primary scope has extremely low *direct* `std::` usage — Core and Detours talk to STL mostly via the UBA alias layer in `UbaMemory.h` (`Vector`, `UnorderedMap`, `UnorderedSet`, `Map`, `Set`, `MultiMap`, `List`, `TString`, `Function`). Secondary scope uses both the UBA aliases and some direct `std::` types (mostly `std::string` in ObjectFile parsing and a few `std::sort` calls).

## Section 1 — std:: types

### `std::vector<T>` (via `Vector<T>` alias)

- **Primary**: 2 direct + 26 via alias. **Secondary**: ~378 via alias.
- **Features**: `push_back`, `emplace_back`, `pop_back`, `back`, `size`, `empty`, `begin/end`, `data`, `resize`, `reserve`, `clear`, `operator[]`, range-for, copy/move-ctor, size-ctor, `std::sort(v.begin(), v.end(), cmp)`.
- **Specializations**: `Vector<u8>` (compression/serialization buffers), `Vector<u64>`, `Vector<HMODULE>`, `Vector<Bucket*>`, `Vector<Thread*>`, `Vector<RefCountPtr<Thread>>`, `Vector<HANDLE>`, `Vector<Dirent*>`, `Vector<std::string>`, `Vector<std::pair<StringKey,u64>>`, `Vector<Atomic<bool>*>`.
- **Complexity**: **Moderate**. Standard methods + `std::sort` + one `std::vector<Import, GrowingAllocator<Import>>` in `KernelBase.inl:2574`.
- **Custom allocators used**: `GrowingAllocator<T>`, `GrowingAllocatorNoLock<T>` — arena-backed; required because Detours cannot use global CRT allocator.

### `std::unordered_map<K,V,...>` (via `UnorderedMap` + `GrowingUnorderedMap`)

- **Primary**: 1 direct (`VisitedModules` typedef with GrowingAllocator) + 7 via aliases. **Secondary**: 70.
- **Features**: `find`, `emplace`, `try_emplace`, `insert` (returning pair<iter,bool>), `erase`, `operator[]`, range-for via `.first/.second`, iterator from `find` + `it->second`, `std::move` into emplace, `size`, `clear`.
- **Specializations**: `<const void*, Entry>`, `<int, DetouredHandle>`, `<FileObject*, JobInfo>`, `<HMODULE, TString>`, `<StringKey, FileInfo>` (GrowingUnorderedMap), `<StringKey, Directory>`, `<const wchar_t*, const wchar_t*, HashString, EqualString>` (custom hash+equal), `<u32, List<CasEntry*>>`, `<u64, Vector<StringKey>>`.
- **Complexity**: **Complex**. Custom-allocator support + custom hash/equal + reference stability across emplace (the `g_jobs` iterate-while-move pattern).

### `std::unordered_set<K,...>` (via `UnorderedSet`)

- **Primary**: 2 sites (`UnorderedSet<void*> g_rpcBindings` + `VisitedModules` typedef). **Secondary**: 23.
- **Features**: `insert` (+ `.second` bool), `find`, `erase`, range-for, `emplace`.
- **Specializations**: `<void*>`, `<TString>`, `<u32>`, `<CasKey>`, `<std::string>`.
- **Complexity**: **Moderate** — same shape as unordered_map, just without the value.

### `std::map<K,V,...>` (via `Map`)

- **Primary**: 4 sites in `UbaPlatform.cpp` (single file — `Map<u64, ModuleRec>`). **Secondary**: 14.
- **Features**: `operator[]`, **`lower_bound`** (killer feature, one site), iterator `->first/->second`, `end()`.
- **Complexity**: **Moderate**. The `lower_bound` use can be replaced by sorted `Vector<Pair<K,V>>` + `std::lower_bound`.

### `std::set<K,...>` (via `Set`)

- **Primary**: 1 site (`Set<TString> g_handledLibraries`). **Secondary**: 22.
- **Features**: `insert`, `find`, `contains`, range-for. **Ordering never actually used** in any observed site.
- **Complexity**: **Trivial** — all 22+1 candidates can become `UnorderedSet`.

### `std::multimap<K,V>` (via `MultiMap`)

- **Primary**: 0. **Secondary**: 1 site. Used purely as "insert then iterate in sorted order".
- **Complexity**: **Trivial** — becomes `Vector<Pair<K,V>>` + `std::sort`.

### `std::list<T>` (via `List`)

- **Primary**: 2 sites (`List<TString> fixedLoaderPaths`, `List<SharedMemoryView>`). **Secondary**: ~12.
- **Features**: `push_back`, `emplace_back`, `emplace_front`, range-for, move-construction. **Pointer/iterator stability is relied on** by a few sites (Worker list, `DeferredCasCreation` storing iterators to List nodes).
- **Complexity**: **Complex** for the stability sites; trivial (→ Vector) for the rest.

### `std::basic_string<tchar,...>` / `std::string`

- **`TString`** (Unicode, alias for `std::basic_string<tchar, char_traits<tchar>, Allocator<tchar>>`): 22 primary + 209 secondary. The default string type in UBA.
- **Raw `std::string`**: 1 primary (Oodle decompress buffer) + ~38 secondary (ObjectFile parsers — COFF, ELF, LLVM-IR, ImportLib — operate on 8-bit byte data).
- **Features used**: `(char*, size)` ctor, `(first, last)` range-ctor, `.size()`, `.resize()`, `.data()` write-through, `std::move`, `<` comparison, `.substr()`, `+=`, range-for, `.find()`/npos once.
- **Complexity**: **Complex** — mutable `.data()` write-through is relied on (Oodle decompress path), range-ctor is used.

### `std::atomic<T>`

- **Primary**: 9 direct in `UbaDetoursGo.cpp` + heavy use via `Atomic<T>` alias. **Secondary**: light direct, heavy via alias.
- **Types**: `u32`, `u64`, `int`, `bool`, `const void*`, `Bucket*`, `Table*` — all trivially copyable.
- **Memory orders seen**: `relaxed`, `acquire`, `release`, `acq_rel` — full set.
- **Complexity**: **Trivial** — compiler intrinsics, no library code. **Keep as-is.**

### `std::pair<T,U>`

- **Primary**: 7 direct (all inside `UbaMemory.h` alias definitions). **Secondary**: 4 direct.
- **Features**: Aggregate ctor, `.first/.second`, `std::move`, sort comparator.
- **Complexity**: **Trivial** — replace with `uba::Pair<T,U>`.

### `std::function<T>` (via `Function<T>`)

- **Primary**: 4 sites (only alias defs + ParkingLot). **Secondary**: **193 total across 54 files** — pervasive in Session/Storage/Network/Scheduler callbacks.
- **Features**: Type-erased storage, move-construction, invocation, nullability check, assignment. No `target<T>()`.
- **Gap**: UBA already has `FunctionWithContext<Ret(Args...)>` at `UbaMemory.h:349-415` — non-owning, lifetime-bound, cheaper than `std::function`. Currently used at 4 sites.
- **Complexity**: **Complex** — 193 sites, but 30–50% could switch to `FunctionWithContext` without new infra.

### `std::shared_ptr`, `std::unique_ptr`, `std::weak_ptr`

- `unique_ptr`: **0 uses anywhere**.
- `shared_ptr`: **1 use** (`UbaObjectFileLLVMIR.h:27` — single isolated site).
- `weak_ptr`: **0 uses**.
- UBA uses `RefCountPtr<T>` for shared ownership and raw pointers + new/delete for unique ownership.

### `std::move` / `std::forward` / `std::swap`

- **Primary**: ~5 direct. **Secondary**: 40+.
- **Complexity**: **Trivial** — compile-time casts from `<utility>`. **Keep.**

### `std::hash<T>` / `std::equal_to<>` / `std::less<>`

- 11 `std::hash` specializations (`StringKey`, `CasKey`, `SharedMemoryHandle`, `ProcessHandle`). Used as template args and as `std::hash<Key>()(key)` in `UbaHashMap.h`.
- `std::equal_to<>`: 7 (template defaults). `std::less<>`: 3 (template defaults).
- **Complexity**: **Trivial** — replace with `uba::Hash<T>`, `uba::Equal<T>`, `uba::Less<T>`.

### `std::mutex`, `std::condition_variable`, `std::shared_mutex`, `std::unique_lock`

- All used in **exactly one file** — `UbaParkingLot.cpp` + `UbaSynchronization.cpp`, as the fallback path when `UBA_USE_PARKINGLOT` is off.
- **Complexity**: **Moderate** — delete the fallback; make ParkingLot the only path.

### `std::is_same_v`, `std::decay_t`, `std::is_invocable_r_v`, `std::invoke_result_t`, etc.

- ~10 uses total, inside `FunctionWithContext` `requires`-clauses and `ScopeGuard::Execute`.
- **Complexity**: **Trivial** — keep `<type_traits>`, it's compile-time only.

### Types with zero uses

`std::optional`, `std::variant`, `std::tuple`, `std::deque`, `std::array`, `std::bitset`, `std::thread`, `std::future`, `std::promise`, `std::string_view` (has in-house `StringView`), `std::numeric_limits`. Plus zero `<iostream>`/`<fstream>`/`<sstream>`.

## Section 2 — STL algorithms

| Algorithm | Uses | Notes |
|---|---:|---|
| `std::sort` | 12 | All with `v.begin(), v.end()`, half with custom cmp lambda. Replace with `uba::Sort`. |
| `std::binary_search` | 1 | Over fixed array. Trivial hand-roll. |
| `std::max` | 1 | `uba::Max` exists in `UbaHashMap.h` already. |
| `std::min` | 1 | Same — `uba::Min`. |
| Everything else (`find`, `lower_bound`, `copy`, `fill`, `swap`, `equal`, `all_of`, ...) | **0** | Not used. |

## Section 3 — STL headers included

All STL headers in UBA are reached via **`Core/Public/UbaMemory.h`** which includes: `<functional>`, `<list>`, `<map>`, `<set>`, `<string>`, `<type_traits>`, `<unordered_map>`, `<unordered_set>`, `<vector>`. Plus `<atomic>`/`<chrono>`/`<condition_variable>`/`<mutex>`/`<shared_mutex>` in a handful of sync files.

**Zero uses** of: `<iostream>`, `<sstream>`, `<fstream>`, `<array>`, `<deque>`, `<optional>`, `<variant>`, `<span>`, `<tuple>`, `<numeric>`, `<thread>`, `<future>`, `<memory>` (except `<new>` once).

**`UbaMemory.h` is the single physical chokepoint.** Replacing its content deletes most STL instantiations project-wide.

## Section 4 — Existing UBA in-house replacements

| In-house type | Location | Status |
|---|---|---|
| `Vector<T, Alloc>` | `UbaMemory.h:85` | Alias for `std::vector` today |
| `UnorderedMap<K,V,H,E>` / `Set` / `GrowingUnorderedMap` | `UbaMemory.h:79,184` | Aliases for `std::unordered_*` |
| `Map`, `Set`, `MultiMap`, `List`, `TString`, `Function` | `UbaMemory.h:75-92` | Aliases |
| `HashMap<K,V,AllowGrow>` / `HashMap2` | `Common/Public/UbaHashMap.h` | **Real in-house** — chaining, MemoryBlock-backed. Used in CacheServer/CompactTables. Subset API (`Insert`/`Find`/`Erase`, no `operator[]`, no pair iterator). |
| `StringBuffer<N>` / `StringBufferBase` / `StringView` | `Core/Public/UbaStringBuffer.h` | **Complete, in-house**. Rich API (Append, Appendf, Parse, StartsWith/EndsWith/Contains, GetFileName, Prepend, Replace, ...). |
| `Futex`, `ReaderWriterLock`, `CriticalSection` | `Core/Public/UbaSynchronization.h` | **In-house**. ParkingLot-backed (primary) + `std::shared_mutex` fallback. |
| `Event`, `SharedEvent`, `EventSlim` | `Core/Public/UbaEvent.h` | **In-house**. |
| `Atomic<T>` | `UbaSynchronization.h:17` | Thin alias for `std::atomic`. Keep. |
| `Function<T>` | `UbaMemory.h:77` | Alias. |
| `FunctionWithContext<Ret(Args...)>` | `UbaMemory.h:349-415` | **In-house**, non-owning, lifetime-bound. Used only 4 times — underused. |
| `ScopeGuard<Lambda>` | `UbaSynchronization.h:278` | In-house. |
| `RefCountPtr<T>` | (referenced but layout not surveyed) | In-house shared-ownership smart pointer. |

## Section 5 — Consolidation candidates

1. **`Set` → `UnorderedSet`** — ordering never exploited (22+1 sites). Eliminates `<set>`.
2. **`Map` → sorted `Vector<Pair>` + `uba::LowerBound`** — the one `lower_bound` site is the only reason `Map` exists (3+14 sites). Eliminates `<map>`.
3. **`MultiMap` → sorted `Vector<Pair>`** — single site. Eliminates `<multimap>`.
4. **`List` → `Vector<T>` mostly, `OwningVector<T>` for pointer-stability sites** (Worker list, SharedMemoryView list, DeferredCasCreation::sources). Eliminates `<list>`.
5. **`std::string` → `TString` or `AnsiString` + `AnsiStringView`** — 38 sites in ObjectFile parsers.
6. **`std::function` subset → `FunctionWithContext`** — ~30–50% of 193 sites.
7. **`std::shared_ptr` single site → `RefCountPtr` or raw pointer** — eliminates `<memory>`.
8. **`std::shared_mutex`/`std::mutex`/`std::condition_variable` fallback → delete, make ParkingLot mandatory** — eliminates 3 heavy STL headers.

## Section 6 — Summary table

Sorted by absolute usage, descending:

| Type | Primary | Secondary | UBA replacement? | Effort | Consolidate? |
|---|---:|---:|---|---|---|
| `std::vector<T>` | 26 | 378 | Alias | **L** | No — keep as Vector |
| `std::atomic<T>` | 100+ | 30+ | Alias | S (keep std) | No |
| `std::basic_string` / `TString` | 72 | 209 | Alias | **L** | No — keep as TString |
| `std::function` | 4 | 193 | `FunctionWithContext` exists | M | Partial |
| `std::unordered_map` | 8 | 70 | `HashMap` for subset | **L** | Partial |
| `std::string` (ANSI) | 1 | 38 | None | M | Yes (→ TString or AnsiString) |
| `std::unordered_set` | 2 | 23 | Alias | M | No |
| `std::set` | 1 | 22 | Alias | S | **Yes → UnorderedSet** |
| `std::map` | 4 | 14 | Alias | M | **Yes → sorted Vector<Pair>** |
| `std::list` | 2 | 12 | Alias | M | **Yes → Vector or OwningVector** |
| `std::sort` | 1 | 11 | None | M | No (build uba::Sort) |
| `std::shared_mutex` | 8 (1 file) | 0 | ReaderWriterLock exists | S | **Yes — delete fallback** |
| `std::mutex`/`cv`/`unique_lock` | 7 (1 file) | 0 | ParkingLot exists | S | **Yes — delete fallback** |
| `std::pair` | 7 | 4 | None | S | No (build uba::Pair) |
| `std::hash`/`equal_to`/`less` | 14 | transitive | — | S | No (rename to uba::) |
| `std::shared_ptr` | 0 | 1 | RefCountPtr | S | **Yes — delete site** |
| `std::multimap` | 0 | 1 | Alias | S | **Yes → Vector<Pair>** |
| `std::max`/`std::min`/`std::invoke`/`std::binary_search` | 4 | 0 | `uba::Max` exists | S | No |
| `std::move`/`std::forward`/`std::swap` | 5 | 40+ | keep | S (keep std) | No |
| `<type_traits>` (`is_same_v`, `decay_t`, ...) | 10 | 2 | keep | S (keep std) | No |
| `std::unique_ptr`/`weak_ptr`/`optional`/`variant`/`tuple`/`deque`/`array` | 0 | 0 | — | 0 | n/a |

## Section 7 — Recommendations

### Build in `UbaContainers.h` (keepers)

1. **`uba::Vector<T, Alloc = Allocator<T>>`** — allocator-aware, contiguous, growth-doubling, trivially-relocatable-aware. Support `GrowingAllocator`, `GrowingAllocatorNoLock`. Features: ctor(count), ctor(initializer_list), ctor(first,last), copy/move, push_back, emplace_back, pop_back, back/front, size, empty, reserve, resize, clear, `operator[]`, begin/end, data, insert(pos,val), erase(pos).
2. **`uba::UnorderedMap<K,V,H,E,A>`** and **`uba::UnorderedSet<K,H,E,A>`** — open-addressing (swiss-table or robin-hood), allocator-aware, custom hash/equal support. Same call-site surface: find, emplace, try_emplace, insert, erase, `operator[]`, iterator pairs.
3. **`uba::Pair<T,U>`** — trivial aggregate, needed by the maps.
4. **`uba::Hash<T>` / `uba::Equal<T>` / `uba::Less<T>`** — functor primitives + specializations for `StringKey`, `CasKey`, `SharedMemoryHandle`, `ProcessHandle`.
5. **`uba::Sort(first, last, cmp)`** — introsort. **`uba::BinarySearch`** / **`uba::LowerBound`** — one site each.
6. **Grow `TString`** into in-house, OR — better — replace with a small-string-optimized `OwnedString` backed by `Allocator<tchar>`.
7. **Keep `StringBuffer`/`StringView`** (already complete).

### Eliminate via consolidation

- **Delete aliases**: `Map`, `Set`, `MultiMap`, `List`. Migrate usages.
- **Delete the `<shared_mutex>`/`<mutex>`/`<condition_variable>` fallback** in Sync + ParkingLot. Make ParkingLot mandatory.
- **Delete `std::shared_ptr`** single-site use in `UbaObjectFileLLVMIR.h`.
- **Migrate some `Function<>` to `FunctionWithContext`** (already exists — just underused).

### Keep as `std::`

- **`std::atomic<T>`** and `std::memory_order_*` — compiler intrinsics.
- **`std::move`, `std::forward`, `std::swap`** — compile-time casts.
- **`<type_traits>`** — `is_same_v`, `decay_t`, `invoke_result_t` — header is lightweight.

### Key observations

1. **UbaMemory.h is the physical chokepoint.** Every TU pulls 8+ STL headers through it. Replacing its content is the single highest-leverage change.
2. **Detours is already 95% non-STL.** Only `std::atomic` (Go-runtime signal worker, must be freestanding), 3x `std::move`, and one `std::vector<Import, GrowingAllocator<Import>>`.
3. **`HashMap`/`HashMap2` already exists in-house** at `Common/Public/UbaHashMap.h`. Refactor should generalize this rather than start from scratch.
4. **No `std::unique_ptr` / `std::optional` / `std::variant` / `std::tuple` / `std::deque` / `std::array` / `std::bitset` anywhere.** Don't build replacements.
5. **No iostream subsystem anywhere.** Don't regress.
6. **Custom allocators are a hard requirement.** ~6 call sites wire `GrowingAllocator` into containers. Any in-house `Vector`/`UnorderedMap` must be allocator-aware from day one.
7. **`std::function` already has a partial in-house replacement** (`FunctionWithContext`) — just underused. Finishing that migration halves the `std::function` footprint without new infra.
8. **Pointer-stability is relied on in a few spots** — Worker list, Connection list, UnorderedMap iterate-while-move. Document and preserve when building replacements.
