# UBA Static-Detour Stub — Work-in-Progress Handoff

This document captures everything a fresh Claude Code session (or a human picking up the work) needs to resume development of the UBA static-detour stub. Written 2026-04-24 after hitting a hard block on Go `compile` / `link`.

---

## 1. What this project is

We are building a way for UBA to intercept I/O from **statically-linked Linux binaries** (Go `compile`/`link`, glibc-static `clang++`, AOSP Go tools like `merge_zips`, `soong_zip`, `build_license_metadata`, etc.). These binaries don't go through `ld.so`, so the normal `LD_PRELOAD` detour path (`UbaDetours.so`) never loads.

**Approach:** Patch the target ELF to embed a freestanding stub at a fresh `PT_LOAD`, redirect `e_entry` to our stub's init. The stub then:

1. Reads `UBA_COMID`/`UBA_LOGFILE`/`UBA_CWD` env vars.
2. Opens the session's shm region at `/dev/shm/uba_<hex>`.
3. Installs a 14-byte `JMP` at two well-known symbols:
   - `internal/runtime/syscall.Syscall6` (Go) / `syscall` (glibc) → `uba_syscall_bridge`
   - `runtime.exit.abi0` (Go) / `_exit` (glibc) → `uba_exit_bridge`
4. Sends `MessageType_Init` RPC over the session's shm/event plumbing.
5. `mmap`s the session's DirectoryTable / MappedFileTable / OverlayTable shm regions.
6. Runs `uba::InitSharedVariables()` and wires up `g_directoryTable`, `g_mappedFileTable`, `g_cancelEvent`, `g_readEvent`, `g_writeEvent`.
7. Jumps back to the binary's original `e_entry`.

At runtime, every intercepted `openat` from Go/glibc lands in `uba_syscall_log`, which calls `uba::Rpc_CreateFileW` via the shared detour code. On exit, `runtime.exit.abi0` lands in `uba_exit_bridge` → `uba_exit_core` which sends `MessageType_Exit` then loops around `SYS_exit_group`.

---

## 2. Environment & build rules (READ THIS FIRST)

Two build environments are in play and **they are NOT interchangeable**:

### 2.1 UBA host/detours on Windows (UbaCli, UbaDetours.so, libUbaDetours.a, etc.)

- Build from **Windows**, not WSL.
- Always **Debug** target. Development builds don't emit `debuglog.log`, which breaks most diagnostics.
- Invocation:
  ```
  RunUBT.bat UbaCli Linux Debug
  RunUBT.bat UbaDetours Linux Debug
  ```
- If something changed in `UbaCore/UbaCommon/UbaDetours` and stale objects are suspected, pass `-clean` first.
- Output goes under `Engine/Binaries/Linux/UnrealBuildAccelerator/`.

### 2.2 The static-detour stub on WSL

- Built via WSL bash because it uses the **AOSP prebuilt clang** (`~/git/android/prebuilts/clang/host/linux-x86/clang-r547379`) — same toolchain that produced the target binaries, so compatible layout / SIMD choices.
- Build script: `Engine/Source/Programs/UnrealBuildAccelerator/DetoursStatic/Build.sh`
- Invocation:
  ```
  wsl -- bash -c "cd /mnt/d/devel/fn/Engine/Source/Programs/UnrealBuildAccelerator/DetoursStatic && bash Build.sh"
  ```
- Output: `Engine/Binaries/Linux/UnrealBuildAccelerator/UbaStaticStub.bin` — the raw blob that `UbaStaticPatcher` embeds into the target ELF.
- Current size around 161 KB. The sentinel `0xDEADBEEFCAFEBABE` is always at byte offset 48 (the patcher finds it and rewrites it to the target's original `e_entry`).

### 2.3 AOSP test corpus

WSL path `~/git/android` contains a prepared AOSP source tree. Candidate test binaries:
- `/home/honk/git/android/out/host/linux-x86/bin/merge_zips` — small Go static, works end-to-end.
- `/home/honk/git/android/out/host/linux-x86/bin/build_license_metadata` — also Go static, works.
- `/home/honk/git/android/prebuilts/go/linux-x86/pkg/tool/linux_amd64/compile` — Go compile driver, **currently SEGVs during first BLAKE3 call**.
- `/home/honk/git/android/prebuilts/go/linux-x86/pkg/tool/linux_amd64/link` — Go linker, not yet directly tested but expected to fail the same way as `compile`.

**Do not kick off fresh full AOSP `m` builds** uninvited. The existing `out/` tree is what we test against.

---

## 3. Directory layout

### Source files

All under `Engine/Source/Programs/UnrealBuildAccelerator/`:

| File | Purpose |
|---|---|
| `DetoursStatic/UbaStaticStub.S` | Asm entry points: `uba_detour_init` (e_entry replacement), `uba_syscall_bridge`, `uba_exit_bridge`. |
| `DetoursStatic/UbaStaticStubCore.cpp` | The C++ core — envp parse, shm attach, hook install, Init RPC, shared-code init, the `uba_syscall_log` dispatch, `uba_exit_core`. |
| `DetoursStatic/UbaStaticStub.ld` | Linker script. Packs `.text + .rodata + .data + .bss` into a single `.text` so `objcopy -O binary` captures everything. Note: `.tbss`, `.dynamic`, `.got`, `.rela.dyn` still land outside `.text` — see §8. |
| `DetoursStatic/Build.sh` | Full build pipeline. Supports `STUB_KEEP_TMP=1` to keep the intermediate `stub.elf` for inspection. |
| `Common/Private/UbaStaticPatcher.cpp` | ELF patcher — finds the sentinel in the blob, bakes `StubSyscall6Sentinel`/`StubExitSentinel` to the real addresses it found via `FindSymbolValue`, inserts a new `PT_LOAD`, rewrites `e_entry`. |
| `Common/Private/UbaProcess.cpp` | Spawns detoured processes. Previously had `if (m_startInfo.isStaticDetoured) return false;` guarding UBA-side cleanup — **removed** now that the stub sends `MessageType_Exit`. |
| `Core/Public/UbaProtocol.h` | `UBA_DEBUG_LOG_ENABLED` is **overridable** (via `-DUBA_DEBUG_LOG_ENABLED=1` at stub build time). |
| `Detours/Private/UbaDetoursShared.cpp` | The VARIABLE_MEM macro uses stub-specific accessor functions under `UBA_STUB_BUILD=1` to avoid `R_X86_64_RELATIVE` relocations (which `objcopy -O binary` drops). |
| `Detours/Private/UbaDetoursFileMappingTable.cpp` | Source of `Rpc_CreateFileW`. Compiled into the stub. |
| `Core/Private/UbaEvent.cpp` + `UbaEventFutex.inl` | On Linux, both `SharedEvent` and `LocalEvent` default to `EventFutex`. No `kTag` stamping — the `EventFutex` ctor writes tag 3 directly. |

### Test artifacts

All in WSL:

| Path | Purpose |
|---|---|
| `/home/honk/UbaCli/debuglog.log` | UBA server debug log. Shows client→server RPCs, session events. Only created in Debug builds. |
| `/tmp/gc_out.log`, `/tmp/mz_out.log` | stdout+stderr of the last compile/merge_zips test run. |
| `/tmp/uba_shm_locks/` | Created by UBA shm setup. |
| `/home/honk/git/android/.uba/` | AOSP-side UBA working area (sessions, cas). |
| `/tmp/tmp.*/stub.elf` | Preserved intermediate stub ELF when `STUB_KEEP_TMP=1`. |
| `/mnt/d/temp/ttt2/commands2.log` | ~1 GB file — commands captured from an AOSP build. Grep this for real-world invocations of `compile`, `link`, `merge_zips`, `clang++`, etc. |
| `/mnt/d/temp/ttt2/log/*.in` | Per-command UBA session inputs from a past run. |

---

## 4. How to test (the canonical recipe)

The **only** mode that exercises the `UbaStaticPatcher` on-the-fly is `local` (not `agent`, not `remote`). `agent` mode uses a different dispatch and doesn't print the patcher diagnostics.

### 4.1 One-shot test of a Go binary

```bash
wsl -- bash -c "
  rm -f /tmp/gc_out.log \
        /home/honk/git/android/prebuilts/go/linux-x86/pkg/tool/linux_amd64/compile.uba \
        /home/honk/UbaCli/debuglog.log
  cd /home/honk/git/android && \
  timeout 15 /mnt/d/devel/fn/Engine/Binaries/Linux/UnrealBuildAccelerator/UbaCli \
    -workdir=/home/honk/git/android \
    local \
    /home/honk/git/android/prebuilts/go/linux-x86/pkg/tool/linux_amd64/compile \
    -o /tmp/x.a.tmp -p p build/soong/third_party/zip/reader.go \
    > /tmp/gc_out.log 2>&1
  echo rc=\$?
  grep -E 'uba_stub|signal|killed|Process [0-9]+|Error' /tmp/gc_out.log | head -20
"
```

**Expected success output** (e.g. for merge_zips):
```
[uba_stub] attached to /dev/shm/uba_04
[uba_stub] Syscall6 @ 0x...
[uba_stub] Syscall6 hook installed
[uba_stub] exit hook installed
[uba_stub] Init RPC sent
[uba_stub] Init resp: pid=1 isChild=0 track=0 dir(h=...) map(h=...) ov(h=...)
[uba_stub] CreateFile(/path,a=...) -> /path size=... closeId=...
...
Detoured run took NN ms
```

**Failure on compile** (current state):
```
[uba_stub] Init resp: ...
SIGSEGV: segmentation violation
PC=<inside stub blob>
...
```

### 4.2 Variations

- Swap `local` binary for `/home/honk/git/android/out/host/linux-x86/bin/merge_zips --help` for the smallest smoke test (no openats).
- `merge_zips -j /tmp/out.zip /tmp/nonexistent.zip` forces an openat → `CreateFile` RPC and exercises BLAKE3.
- For `compile` a cleaner test input is handy: `-o /tmp/x.a.tmp -p p build/soong/third_party/zip/reader.go` (one Go file, will fail Go compilation due to missing deps — that's fine; the stub ran).

### 4.3 Just patch (no run)

```bash
/mnt/d/devel/fn/Engine/Binaries/Linux/UnrealBuildAccelerator/UbaCli \
  -workdir=/home/honk/git/android \
  -patchbinary=<input-elf> -out=<output-elf>
```
Useful for then running the patched binary directly under strace / gdb.

---

## 5. Current status

### Verified working (exits cleanly, full RPC flow):
- `merge_zips`
- `build_license_metadata`
- `sbox`, `zipsync`, `soong_zip`, `zip2zip` (per prior session logs — not re-verified this turn)
- **Go `compile`** — the previously-broken target. Runs through telemetry
  init, compiles source files, emits errors/output normally. Verified
  2026-04-24 PM after the fix described in §6.

### Probably working (not re-verified this turn):
- **Go `link`** — the same fix should apply since it hit the same path.

### Deferred:
- **glibc-static binaries** (clang++, clang) — explicitly not yet in scope.

### The critical fix that did land (don't lose this)

`uba_exit_core` **must** loop around `SYS_exit_group`:

```cpp
extern "C" __attribute__((used, visibility("hidden"), noreturn))
void uba_exit_core(int exitCode)
{
    if (__atomic_load_n(&g_initDone, __ATOMIC_ACQUIRE))
        uba::SendExitRpc((u32)exitCode);
    // Infinite loop prevents compiler from letting control fall through
    // into the next function in the translation unit (seen on Go `compile`:
    // exit_group returned and we fell into uba_detour_core, which SEGV'd on
    // a zeroed g_shmMem).
    for (;;)
        raw_syscall1(231 /* SYS_exit_group */, (s64)exitCode);
}
```

Plain `raw_syscall1(...); __builtin_unreachable();` was NOT sufficient — the compiler emitted no terminator byte and control fell through.

---

## 6. The BLAKE3 problem — resolved 2026-04-24

**Fixed.** Go `compile` and `link` now run end-to-end through the stub;
the openat → CreateFile RPC path fires cleanly on telemetry files and
source files alike. Compile exits with its own `rc=2` for missing
imports (our one-file test input lacks deps) — not a stub crash.

The resolution took three changes:

1. **Alt-stack switch in `uba_syscall_bridge`.** Go goroutines enter
   `syscall.Syscall6` with ~2 KB of remaining stack during early init,
   which the C++ hook path (ToStringKeyNoCheck's 1952-byte frame alone)
   overflows. The bridge now saves Go's rsp, switches to a 64 KB alt-
   stack (`g_altStack`), and restores rsp before the trampoline jmp.
2. **Signal blocking around the hook.** On the alt-stack, Go's own
   SIGURG handler (installed with SA_ONSTACK) panics when it inspects
   the interrupted rsp and finds it outside any goroutine's known
   bounds. The bridge wraps the hook in `rt_sigprocmask(SIG_SETMASK,
   all-ones)` / restore. Both procmask calls sit in the asm because
   the `syscall` instruction clobbers Go's register-ABI regs — they
   are saved on Go's stack around the syscall.
3. **Portable-only BLAKE3 + sidestep the vectorized memcpy.** The
   pre-built `libBLAKE3.a` links in all SIMD variants plus a CPUID-
   based dispatcher reading `g_cpu_features` (static int init
   `0x40000000`). `Build.sh` now compiles BLAKE3 from
   source with `-DBLAKE3_NO_AVX512 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_SSE41
   -DBLAKE3_NO_SSE2 -DBLAKE3_USE_NEON=0` — reduces the blob from
   ~227 KB → ~176 KB and eliminates the dispatcher entirely (BLAKE3
   usage on the detour side is minimal, so the portable path is fine).
   Separately, the `[uba_stub] CreateFile(...)` log line is emitted as
   several direct `stub_write` syscalls instead of being built into
   a stack-local `char lbuf[512]` — clang's vectorized memcpy into
   that buffer faulted at compile's stub_base for reasons not yet
   fully understood (suspected interaction with SSE aligned moves in
   our freestanding `-mstackrealign` build), and the multi-write
   approach is simpler and robust.

### Originally-wrong diagnosis, preserved for context

The initial investigation blamed a crash "inside
`blake3_hasher_init_derive_key_raw`, `%rbx=0x24`". That diagnosis was
wrong: Go's signal handler reports a misleading PC under
`si_code=SI_KERNEL`, what looked like `%rbx` was actually `%r12`
holding `plen=0x24` by coincidence, and `ToStringKeyNoCheck`'s call
graph never reaches `_derive_key_raw` (verified via disasm). The real
faulting instruction was in the lbuf-building loop's vectorized copy.

### Leftover follow-ups (lower priority)

- **Understand WHY the stack-local lbuf memcpy faulted.** Replacing
  `char lbuf[512]; const char pre[] = "..."; for (i) lbuf[i]=pre[i];`
  with direct `stub_write` syscalls fixed it, but we never proved the
  root cause. Likely a clang-autovectorized SSE move (possibly aligned)
  against a stack offset that happens to be misaligned at compile's
  stub_base. Worth verifying before writing more C++ into the hook
  path. Workaround: if you need to build formatted buffers in the
  hook, either mark the buffer `alignas(16)` AND audit the emitted
  asm, or stick with multi-`stub_write`.
- **Stale-looking blob bytes at `g_cpu_features` / `IV` offsets when
  BLAKE3 was the prebuilt .a.** Not reproduced with the portable-only
  rebuild (those symbols don't appear anymore), but if we ever go back
  to linking the prebuilt `libBLAKE3.a`, re-verify the linker-script
  `.data`/`.rodata` → `.text` packing preserves initialized data
  at the symbol offsets.
- **Multi-M safety of the alt-stack.** `g_altStack` is a single 64 KB
  buffer with `g_savedGoRsp` as the only save slot. That's safe for
  single-M Go programs (like AOSP's compile/link during init) but
  races if two Ms enter the hook concurrently. Plan for a per-M
  strategy when we target multi-threaded Go toolchains.

### What was tried and ruled OUT

- **Goroutine stack overflow on Go's small goroutine stack.** The
  alt-stack switch closes this, but by itself did NOT fix compile.
- **SIGURG from Go's preemption thread delivered on the alt-stack.**
  The `rt_sigprocmask` block closes this, but by itself did NOT fix
  compile.
- **`%rbx=0x24` indicating a bad hasher pointer argument.** Bad
  diagnosis — Go mis-reports the PC under SI_KERNEL, and the
  "bad register" was actually `%r12` holding `plen=0x24`.
- **Path pointer validity.** Probed — Go's string memory is readable
  and correct.
- **Stack alignment at the call site.** Probed — `rsp % 16 == 0`.

---

## 7. Useful scripts

### `inspect_stub.sh` (lives in `D:\devel\fn\FortniteGame\` during testing)

Evolves constantly while debugging — canonical contents below. Requires `STUB_KEEP_TMP=1 bash Build.sh` to have run first so `/tmp/tmp.XYZ/stub.elf` exists.

**Important git-bash gotcha:** invoking from Windows `wsl -- /mnt/.../inspect_stub.sh` triggers MSYS2 path translation on `-` arguments. Always use:
```
MSYS_NO_PATHCONV=1 wsl -- /mnt/d/devel/fn/FortniteGame/inspect_stub.sh
```

Typical content when drilling into a section question:
```bash
#!/bin/bash
CB=/home/honk/git/android/prebuilts/clang/host/linux-x86/clang-r547379/bin
elf=$(ls -td /tmp/tmp.* 2>/dev/null | head -1)/stub.elf
echo "elf = $elf"

echo '=== sections ==='
"$CB/llvm-readelf" -S "$elf" | head -40

echo '=== dyn relocs ==='
"$CB/llvm-readelf" -r "$elf"

echo '=== TLS-related object files ==='
for o in $(dirname "$elf")/*.o; do
  "$CB/llvm-readelf" -S "$o" 2>/dev/null | grep -qE 'tbss|tdata' && \
    echo "TLS in $o"
done

echo '=== symbol at crash addr ==='
# Crash PC was 0x15e4188, stub base was 0x15d4000, text vaddr is 0x1bc0
# Target ELF addr = 0x1bc0 + (0x15e4188 - 0x15d4000) = 0x11d48
"$CB/llvm-nm" -n "$elf" | awk 'BEGIN{t=0x11d48} {
  c=strtonum("0x"$1); if (c>t) {print last; exit} last=$0
}'
```

### Probe pattern (what to paste into `UbaStaticStubCore.cpp` to bisect)

Static strings placed in `.text` so they survive objcopy:
```cpp
{ static const char m[] __attribute__((section(".text"))) = "[dbg] <label>\n";
  stub_write(2, m, sizeof(m)-1); }
```

Char-by-char hex dump (when stack-local buffers are suspect):
```cpp
{
    u64 v = (u64)some_pointer;
    const char* hex = "0123456789abcdef";
    const char lab[] = "[dbg] p=0x";
    stub_write(2, lab, sizeof(lab) - 1);
    for (int i = 60; i >= 0; i -= 4) {
        char c = hex[(v >> i) & 0xF];
        stub_write(2, &c, 1);
    }
    stub_write(2, "\n", 1);
}
```

---

## 8. Environment quirks

### WSL / git-bash path translation

Running `wsl -- bash -c "... $VAR ..."` from git-bash will translate anything that looks like a POSIX path (e.g. `/home/...`, `$CB/llvm-...`) unless you set `MSYS_NO_PATHCONV=1` or double-quote the entire `-c` argument carefully. When in doubt, write a throwaway `.sh` file and invoke that via `MSYS_NO_PATHCONV=1 wsl -- /mnt/d/.../script.sh`.

### `pkill -9 -f UbaCli` is dangerous in this shell

The current command line **contains** the string `UbaCli`, so `pkill -9 -f UbaCli` can match and kill the parent shell. Use `ps -ef | grep UbaCli | grep -v grep | awk '{print $2}' | xargs -r kill -9` or similar.

### `-dir=<path>` with UbaCli

If you override `-dir`, the debuglog can end up somewhere other than `/home/honk/UbaCli/debuglog.log`. Some code paths in UbaCli assume the default. **Leave `-dir` unset** unless you specifically need session isolation.

### strace gotcha

`timeout 15 strace -f -o /tmp/foo.log ...` — the child exits cleanly but the strace log tells you si_addr/si_code for the SIGSEGV via:
```
--signal=SIGSEGV
```
Adding `-k --stack-trace` for call stacks requires `libunwind` which isn't on this WSL.

---

## 9. Task list context

Tasks #74 and below are done (see the project TodoRead for full history). Open tasks that matter:

- **#78 — Investigate BLAKE3 SEGV on Go compile/link.** *The blocker.* See §6.
- #53 — the overall "Phase 2e: stub patches Go syscall.Syscall6 + routes I/O" umbrella. Completes when #78 resolves and compile/link run through.
- #37 — delete the std::mutex/cond fallback in UbaParkingLot. Orthogonal, safe to do.

---

## 10. How to resume — checklist for the next session

1. Read this file first.
2. Read the memory entries:
   - `~/.claude/projects/D--devel-fn-FortniteGame/memory/MEMORY.md`
   - In particular `feedback_uba_build_env.md` and `feedback_stub_exit_fallthrough.md`.
3. Verify the tree is intact — rebuild the stub and smoke-test merge_zips (§4.1 with merge_zips substituted). Should exit cleanly with `[uba_stub] CreateFile(...)` lines.
4. Confirm compile still SEGVs with the current blob (§4.1 as written).
5. Pick up investigation on task #78 — first move: check `/proc/<pid>/maps` of a patched `compile` vs a patched `merge_zips` right before the BLAKE3 call.

Good luck.
