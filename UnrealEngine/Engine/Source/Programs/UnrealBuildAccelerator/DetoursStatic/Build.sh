#!/usr/bin/env bash
#
# Compile UbaStaticStub.S and produce the raw blob binary.
#
# Output lands at:
#   Engine/Binaries/Linux/UnrealBuildAccelerator/UbaStaticStub.bin
#
# UbaStaticPatcher reads this file at runtime, finds the sentinel
# 0xDEADBEEFCAFEBABE in it, and embeds the bytes into the target ELF
# after patching the sentinel to the target's original e_entry.
#
# Run manually whenever the .S source changes. The .bin is checked in
# to p4 alongside the other UBA Linux binaries.
#
# Invoke from Linux (or WSL):
#   bash Build.sh
#
# Environment overrides:
#   CLANG, CLANGXX, LD, OBJCOPY, LLVMNM  — toolchain paths
#     (default: AOSP prebuilt clang under ~/git/android/prebuilts/clang/;
#      set to system clang/lld/llvm binaries for a clean-machine build —
#      see Readme.txt).

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
engine_root=$(cd -- "$script_dir/../../../../.." && pwd)

src_s="$script_dir/UbaStaticStub.S"
src_cpp="$script_dir/UbaStaticStubCore.cpp"
# Consolidated glibc-static detour TU — prototypes, handler bodies
# (real + abort-on-reach for unsupported ops), fd-table, and the
# GlibcHandlerTable instance that the patcher walks at injection time.
# The linker script (UbaStaticStub.ld) routes the table into
# .text.uba_handlers so it lands near the head of the blob; the
# patcher locates it by scanning for kGlibcHandlerTableMagic.
src_glibc="$script_dir/UbaGlibc.cpp"
core_private="$engine_root/Engine/Source/Programs/UnrealBuildAccelerator/Core/Private"
detours_private="$engine_root/Engine/Source/Programs/UnrealBuildAccelerator/Detours/Private"
src_hash="$core_private/UbaHash.cpp"
# Shared UBA core sources compiled into the stub. These provide MemoryBlock,
# Futex, CriticalSection, ReaderWriterLock, ParkingLot primitives used by
# DirectoryTable / MappedFileTable.
src_shared_core=(
    "$core_private/UbaMemory.cpp"
    "$core_private/UbaParkingLot.cpp"
    "$core_private/UbaStringBuffer.cpp"
    "$core_private/UbaSynchronization.cpp"
    "$detours_private/UbaDetoursFileMappingTable.cpp"
    "$detours_private/UbaDetoursShared.cpp"
)
# Intentionally NOT compiled for the stub:
#   - UbaSharedMemoryView.cpp — Linux impl is a stub; Rpc_*SharedMemory paths
#     in UbaDetoursFileMappingTable.cpp are Windows-only in practice.
#   - UbaEvent.cpp — pulls UbaEventPThread.inl which drags pthread types. The
#     stub provides its own SharedEvent::Set / SharedEvent::IsSet (tag-3 only)
#     since the session creates EventFutex for static-detoured children.
# BLAKE3 is recompiled from source (not the prebuilt libBLAKE3.a) with
# all SIMD variants disabled. Reason: the prebuilt .a contains the
# CPUID-based dispatcher in blake3_dispatch.c which reads a file-scope
# static `g_cpu_features = UNDEFINED (0x40000000)`. That sentinel lives
# in `.data`. When our linker script (UbaStaticStub.ld) packs
# .data into .text and `objcopy -O binary --only-section=.text` extracts
# the blob, the merge has worked fine at low stub_base addresses but
# failed reproducibly at high addresses (e.g. Go `compile` at 0x15d4000)
# with SIGSEGV/SI_KERNEL — suggesting an address-dependent resolution
# of something in the SIMD dispatcher or its embedded constants.
# Forcing the portable-only path (no g_cpu_features read, no SIMD
# dispatcher, no per-variant .rodata) eliminates the class of bugs.
blake3_inc="$engine_root/Engine/Source/ThirdParty/BLAKE3/1.3.1/c"
blake3_srcs=(
    "$blake3_inc/blake3.c"
    "$blake3_inc/blake3_dispatch.c"
    "$blake3_inc/blake3_portable.c"
)
# Defines that stub out each SIMD compile target inside blake3_dispatch.c.
# With all three NO_* set, the dispatcher's tail-jumps reduce to a single
# fall-through into the portable path and `g_cpu_features` is never read.
BLAKE3_NO_SIMD=(
    -DBLAKE3_NO_AVX512
    -DBLAKE3_NO_AVX2
    -DBLAKE3_NO_SSE41
    -DBLAKE3_NO_SSE2
    -DBLAKE3_USE_NEON=0
)
out_dir="$engine_root/Engine/Binaries/Linux/UnrealBuildAccelerator"
out_bin="$out_dir/UbaStaticStub.bin"

CLANG=${CLANG:-/home/honk/git/android/prebuilts/clang/host/linux-x86/clang-r547379/bin/clang}
CLANGXX=${CLANGXX:-/home/honk/git/android/prebuilts/clang/host/linux-x86/clang-r547379/bin/clang++}
LD=${LD:-/home/honk/git/android/prebuilts/clang/host/linux-x86/clang-r547379/bin/ld.lld}
OBJCOPY=${OBJCOPY:-/home/honk/git/android/prebuilts/clang/host/linux-x86/clang-r547379/bin/llvm-objcopy}
LLVMNM=${LLVMNM:-/home/honk/git/android/prebuilts/clang/host/linux-x86/clang-r547379/bin/llvm-nm}

for tool in "$CLANG" "$CLANGXX" "$LD" "$OBJCOPY"; do
    if [[ ! -x "$tool" ]]; then
        echo "ERROR: missing tool: $tool" >&2
        exit 1
    fi
done

mkdir -p "$out_dir"

tmp=$(mktemp -d)
if [[ "${STUB_KEEP_TMP:-0}" != "1" ]]; then
    trap 'rm -rf "$tmp"' EXIT
else
    echo "=== keeping tmp dir: $tmp ==="
fi

obj_s="$tmp/stub_entry.o"
obj_cpp="$tmp/stub_core.o"
obj_hash="$tmp/uba_hash.o"
obj_glibc="$tmp/uba_glibc.o"
elf="$tmp/stub.elf"

CFLAGS_COMMON=(
    -O2 -nostdlib -ffreestanding -fPIC -fno-stack-protector
    -mno-red-zone -fno-asynchronous-unwind-tables -fno-unwind-tables
    -fvisibility=hidden -fno-plt -Wa,--noexecstack
    # SSE kept ON so libstdc++ headers parse (they need long double etc.),
    # but force the compiler to emit a stack-realign prologue on every
    # function. This prevents the GPF we previously saw on `movaps` when
    # the Go syscall bridge re-entered the stub with a rsp that wasn't
    # 16-byte aligned. Also lie about incoming alignment so the compiler
    # doesn't assume 16-byte from the call site.
    -mstackrealign
    # Signal to shared UBA headers that they're being pulled into the
    # freestanding stub — gate heavy methods behind `#if !UBA_STUB_BUILD`.
    -DUBA_STUB_BUILD=1
    # Turn on the pretty detour log that LD_PRELOAD builds emit
    # (DEBUG_LOG_DETOURED / DEBUG_LOG_TRUE / DEBUG_LOG_PIPE) — not tied to
    # UBA_DEBUG so we don't drag in the full assert machinery.
    -DUBA_DEBUG_LOG_ENABLED=1
    # Compile out every assert(): without NDEBUG, BLAKE3's assertions
    # (and any other assert-using code) emit `callq *<GOT slot>` for
    # __assert_fail.  Our linker script does not capture .got/.dynamic,
    # so the slot reads zero at runtime and the call would jump to 0.
    # Stub-core also defines a strong __assert_fail as belt-and-suspenders.
    -DNDEBUG=1
)

# C++-specific: disable exceptions, RTTI, and thread-safe statics so no
# glibc/libstdc++ symbols sneak in.
CXXFLAGS_STUB=(
    -std=c++20 -nostdlib++ -fno-exceptions -fno-rtti
    -fno-threadsafe-statics
)

echo "=== assemble entry ==="
"$CLANG" "${CFLAGS_COMMON[@]}" -c "$src_s" -o "$obj_s"

echo "=== compile core (c++) ==="
# UBA headers we want to share live under Engine/Source/Programs/UnrealBuildAccelerator/{Core,Common,Detours}/Public
UBA_INCLUDES=(
    -DPLATFORM_LINUX=1 -DPLATFORM_WINDOWS=0 -DPLATFORM_MAC=0
    -I"$engine_root/Engine/Source/Programs/UnrealBuildAccelerator/Core/Public"
    -I"$engine_root/Engine/Source/Programs/UnrealBuildAccelerator/Detours/Private"
    -I"$blake3_inc"
)
"$CLANGXX" "${CFLAGS_COMMON[@]}" "${CXXFLAGS_STUB[@]}" "${UBA_INCLUDES[@]}" -Wall -Werror -c "$src_cpp" -o "$obj_cpp"

echo "=== compile UbaHash.cpp (shared) ==="
"$CLANGXX" "${CFLAGS_COMMON[@]}" "${CXXFLAGS_STUB[@]}" "${UBA_INCLUDES[@]}" -Wall -Werror -c "$src_hash" -o "$obj_hash"

echo "=== compile UbaGlibc.cpp (consolidated handlers + table + fd map) ==="
"$CLANGXX" "${CFLAGS_COMMON[@]}" "${CXXFLAGS_STUB[@]}" "${UBA_INCLUDES[@]}" -I"$script_dir" -Wall -Werror -c "$src_glibc" -o "$obj_glibc"

shared_core_objs=()
for src in "${src_shared_core[@]}"; do
    base=$(basename "$src" .cpp)
    out="$tmp/${base}.o"
    echo "=== compile $base.cpp (shared core) ==="
    "$CLANGXX" "${CFLAGS_COMMON[@]}" "${CXXFLAGS_STUB[@]}" "${UBA_INCLUDES[@]}" -Wall -c "$src" -o "$out"
    shared_core_objs+=("$out")
done

# BLAKE3 recompiled portable-only.
blake3_objs=()
for src in "${blake3_srcs[@]}"; do
    base=$(basename "$src" .c)
    out="$tmp/blake3_${base}.o"
    echo "=== compile BLAKE3 $base.c (portable-only) ==="
    "$CLANG" "${CFLAGS_COMMON[@]}" "${BLAKE3_NO_SIMD[@]}" -I"$blake3_inc" -Wall -c "$src" -o "$out"
    blake3_objs+=("$out")
done

echo "=== link (self-contained shared + custom script) ==="
"$LD" \
    -shared -Bsymbolic --no-dynamic-linker -nostdlib -z noexecstack \
    -e 0 \
    -T "$script_dir/UbaStaticStub.ld" \
    -o "$elf" "$obj_s" "$obj_cpp" "$obj_hash" "$obj_glibc" \
    "${shared_core_objs[@]}" "${blake3_objs[@]}"

echo "=== verify uba_detour_init at offset 0 within .text ==="
# Dump the full symbol table once, then grep — using `awk { exit }` inside a
# pipeline is fragile under `set -euo pipefail` because the upstream tool
# gets SIGPIPE on early termination, fails the pipeline, and silently kills
# the script depending on host awk/binutils behaviour. Dumping once makes
# every per-symbol lookup deterministic.
nm_dump="$tmp/stub_nm.txt"
"$LLVMNM" "$elf" > "$nm_dump"

if [[ -x "$LLVMNM" ]]; then
    init_off=$(awk '$3 == "uba_detour_init" { print $1 }' "$nm_dump" | head -1)
    echo "  uba_detour_init virtual address: 0x${init_off}"
    echo "=== unresolved externs in final ELF ==="
    "$LLVMNM" -u "$elf" || true
fi

echo "=== extract raw .text bytes ==="
"$OBJCOPY" -O binary --only-section=.text "$elf" "$out_bin"

blob_size=$(stat -c '%s' "$out_bin")
echo "=== blob size: $blob_size bytes → $out_bin ==="

# Find the sentinel and report its byte offset, so a human can verify
# the patcher and this script agree on where to rewrite.
sentinel_hex="bebafecaefbeadde"  # 0xDEADBEEFCAFEBABE little-endian
sentinel_off=$(python3 - <<PY
import sys
data = open("$out_bin", "rb").read()
target = bytes.fromhex("$sentinel_hex")
idx = data.find(target)
print(idx)
PY
)

if [[ "$sentinel_off" == "-1" ]]; then
    echo "ERROR: sentinel 0xDEADBEEFCAFEBABE not found in blob" >&2
    exit 2
fi

echo "=== sentinel at byte offset $sentinel_off ==="

# ---------------------------------------------------------------------------
# Post-link GlibcHandlerTable offset stamping (Phase 1.A + Phase 3 import).
#
# The handler table is laid out by the C compiler in UbaGlibcHandlerTable.cpp
# with all entries[i].offset = 0 — addresses are unknown until link time.
# Two entry kinds are stamped here:
#
#   HandlerHook (kind=0): `name` is the stripped libc symbol (open, openat,
#     ...). The matching stub function is uba_glibc_<name>; its VA inside
#     the blob is computed and stamped into `offset`.
#
#   ImportSlot (kind=1): `name` is an exact target symbol the patcher will
#     resolve at injection time. `offset` is the blob-relative location of
#     a writable 8-byte slot the patcher will stamp.
#       - "__errno_location" -> g_glibc_errno_hook_fn
#
# Steps:
#   1. Scan the .bin for kGlibcHandlerTableMagic at u64 alignment to
#      locate the table inside the blob (same lookup the patcher does).
#   2. Use llvm-nm to read each symbol's VA from the linked ELF. Subtract
#      the .text base VA (= the VA that objcopy --only-section=.text mapped
#      to byte 0 of the blob) to get offset_within_blob.
#   3. Patch the per-entry little-endian u32 offsets in the .bin.
# ---------------------------------------------------------------------------

echo "=== stamp GlibcHandlerTable handler offsets ==="

# Discover the .text section's virtual address from the linked ELF. This is
# the base that objcopy used when extracting .text into a flat .bin (i.e.
# the byte at vaddr=text_vaddr in the ELF maps to byte 0 of $out_bin).
LLVMREADELF=${LLVMREADELF:-/home/honk/git/android/prebuilts/clang/host/linux-x86/clang-r547379/bin/llvm-readelf}
if [[ ! -x "$LLVMREADELF" ]]; then
    LLVMREADELF=$(dirname "$LLVMNM")/llvm-readelf
fi

# Same SIGPIPE-safety pattern as the symbol map below: dump once, then awk.
readelf_dump="$tmp/stub_readelf.txt"
"$LLVMREADELF" -S --wide "$elf" > "$readelf_dump"
text_vaddr_hex=$(awk '/^[[:space:]]*\[[[:space:]]*[0-9]+\][[:space:]]+\.text[[:space:]]/ { print $5; exit }' "$readelf_dump")

if [[ -z "$text_vaddr_hex" ]]; then
    echo "ERROR: could not determine .text virtual address of $elf" >&2
    exit 3
fi

echo "  .text vaddr in ELF: 0x$text_vaddr_hex"

# Collect virtual addresses of every symbol the table can reference. We
# look up:
#   - uba_glibc_<name>      for HandlerHook entries
#   - g_glibc_errno_hook_fn for the __errno_location ImportSlot
# The Python step below maps `(name, kind)` to the right symbol.
sym_names_to_lookup=(
    # Real handlers (mapped to libc wrappers via kGlibcAliasTable).
    uba_glibc_open uba_glibc_openat uba_glibc_read uba_glibc_write
    uba_glibc_close uba_glibc_lseek uba_glibc_fxstat uba_glibc_mmap
    uba_glibc_access uba_glibc_exit
    # Abort-on-reach handlers for unsupported glibc operations. Order
    # mirrors the entries[] initialiser in UbaGlibc.cpp (Section 6).
    uba_glibc_stat uba_glibc_lstat uba_glibc_fstatat uba_glibc_statx
    uba_glibc_unlink uba_glibc_unlinkat
    uba_glibc_rename uba_glibc_renameat uba_glibc_renameat2
    uba_glibc_link uba_glibc_linkat
    uba_glibc_symlink uba_glibc_symlinkat
    uba_glibc_readlink uba_glibc_readlinkat
    uba_glibc_truncate uba_glibc_ftruncate
    uba_glibc_mkdir uba_glibc_mkdirat uba_glibc_rmdir
    uba_glibc_fork uba_glibc_vfork
    uba_glibc_clone uba_glibc_clone3
    uba_glibc_execve uba_glibc_execveat
    uba_glibc_dup uba_glibc_dup2 uba_glibc_dup3
    uba_glibc_pipe uba_glibc_pipe2
    uba_glibc_opendir uba_glibc_fdopendir
    uba_glibc_readdir uba_glibc_closedir uba_glibc_getdents64
    uba_glibc_chdir uba_glibc_fchdir
    # Import slot (patcher stamps target's VA here).
    g_glibc_errno_hook_fn
)

sym_map=""
for sym in "${sym_names_to_lookup[@]}"; do
    # Read from the nm dump file rather than re-piping `llvm-nm | awk` per
    # symbol — the latter combines `awk { exit }` with `set -euo pipefail`
    # and gets SIGPIPE-killed silently on some host awks, leaving the blob
    # un-stamped (entries[i].offset = 0) and the patcher then refusing to
    # patch with "Build.sh post-link stamping did not run".
    line=$(awk -v s="$sym" '$3 == s { print $1; exit }' "$nm_dump")
    if [[ -z "$line" ]]; then
        echo "ERROR: missing symbol $sym in $elf" >&2
        exit 4
    fi
    sym_map+="${sym}=${line};"
done

# Compute and stamp offsets via Python (it does the binary surgery and the
# magic search and we don't have to spawn one process per symbol).
#
# Pass the per-symbol map via env vars so embedded special chars never break
# bash quoting. Format: SYM_MAP="name1=hex1;name2=hex2;..."
OUT_BIN="$out_bin" \
TEXT_VADDR_HEX="$text_vaddr_hex" \
SYM_MAP="$sym_map" \
python3 - <<'PY'
import os, struct, sys

bin_path       = os.environ["OUT_BIN"]
elf_text_vaddr = int(os.environ["TEXT_VADDR_HEX"], 16)
sym_map_raw    = os.environ["SYM_MAP"]

sym_to_vaddr = {}
for kv in sym_map_raw.split(";"):
    if not kv:
        continue
    name, vhex = kv.split("=", 1)
    sym_to_vaddr[name] = int(vhex, 16)

# Magic = 'UBAGLBHT' little-endian u64 = 0x5448424C47414255.
MAGIC = 0x5448424C47414255

with open(bin_path, "rb") as f:
    blob = bytearray(f.read())

# Locate the table by scanning for the magic at u64 alignment.
table_off = -1
for i in range(0, len(blob) - 8, 8):
    if struct.unpack_from("<Q", blob, i)[0] == MAGIC:
        table_off = i
        break

if table_off < 0:
    sys.stderr.write("ERROR: GlibcHandlerTable magic not found in {}\n".format(bin_path))
    sys.exit(5)

# Layout: u64 magic, u32 count, u32 reserved, then entries[].
# Each entry is 32 bytes: char name[24], u32 offset, u32 kind.
count = struct.unpack_from("<I", blob, table_off + 8)[0]

entries_off = table_off + 16
ENTRY_SZ    = 32
NAME_SZ     = 24
NAME_OFF    = 0
OFF_OFF     = 24
KIND_OFF    = 28

KIND_HANDLER_HOOK = 0
KIND_IMPORT_SLOT  = 1

# How to find the linker symbol for a given (entry-name, kind):
#   HandlerHook: stub function is uba_glibc_<name>
#   ImportSlot:  bespoke per-name; only __errno_location is wired today
IMPORT_SYM_FOR_NAME = {
    "__errno_location": "g_glibc_errno_hook_fn",
}

print("  table at blob offset 0x{:x} (count={})".format(table_off, count))
print("  stamped offsets:")
for i in range(count):
    e_off  = entries_off + i * ENTRY_SZ
    e_name_raw = bytes(blob[e_off + NAME_OFF : e_off + NAME_OFF + NAME_SZ])
    e_name = e_name_raw.split(b"\x00", 1)[0].decode("ascii")
    e_kind = struct.unpack_from("<I", blob, e_off + KIND_OFF)[0]

    if e_kind == KIND_HANDLER_HOOK:
        sym = "uba_glibc_" + e_name
        kind_label = "hook"
    elif e_kind == KIND_IMPORT_SLOT:
        sym = IMPORT_SYM_FOR_NAME.get(e_name)
        if sym is None:
            sys.stderr.write("ERROR: ImportSlot entry {} ({}) has no symbol mapping in IMPORT_SYM_FOR_NAME\n"
                             .format(i, e_name))
            sys.exit(7)
        kind_label = "import"
    else:
        sys.stderr.write("ERROR: entry {} ({}) has unknown kind {}\n".format(i, e_name, e_kind))
        sys.exit(7)

    if sym not in sym_to_vaddr:
        sys.stderr.write("ERROR: entry {} ({}) needs symbol {} but it was not collected from the ELF\n"
                         .format(i, e_name, sym))
        sys.exit(7)

    sym_vaddr = sym_to_vaddr[sym]
    sym_offset = sym_vaddr - elf_text_vaddr
    if sym_offset < 0 or sym_offset >= len(blob):
        sys.stderr.write("ERROR: entry {} ({}) -> {} offset 0x{:x} outside blob (size 0x{:x})\n"
                         .format(i, e_name, sym, sym_offset, len(blob)))
        sys.exit(8)
    struct.pack_into("<I", blob, e_off + OFF_OFF, sym_offset)
    print("    {:18s} [{}] -> {:24s} +0x{:x}".format(e_name, kind_label, sym, sym_offset))

with open(bin_path, "wb") as f:
    f.write(bytes(blob))
PY

echo "=== done ==="
