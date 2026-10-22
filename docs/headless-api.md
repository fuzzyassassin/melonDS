# melonDS headless C API reference

This is the reference for `src/frontend/headless/melonds_api.h`, the C API exported by
`melonds_headless.dll` (`.so`, `.dylib`). It says what every exported function does, what state it
needs, what it blocks on and what it returns, so it can be used without reading the implementation.

Every claim here was taken from the code (`src/frontend/headless/*`, `src/debug/Debugger.*`, the
CPU and DMA hook sites in `src/ARM*.cpp`, `src/CP15.cpp`, `src/DMA.cpp`) or from the tests
(`src/frontend/headless/tests`, `tools/melonds-mcp/tests`). Claims marked *(inferred)* follow from
reading the code but no test covers them. Claims marked *(verified)* were checked with a small
one-console experiment against this branch's DLL. Bugs and surprises are collected at the end, in
[Known issues and surprises](#11-known-issues-and-surprises).

This revision describes **API level 9**: the `fix/api-quirks` changes (level 2), Action Replay
cheat codes from the `feature/cheats` branch (level 3), the debugger work merged from
ZevyaDev's `debug` branch on `merge/upstream-debug` (level 4: thread stacks, the new call tracker,
PC changes at a break, pipeline refetch, Lua `memory.execute` and `wifi`), its next merge on
`merge/upstream-debug-2` (level 5: `mds_overlays`, the GDB stub's session handling, extended mode
and `monitor overlays`, DWARF line info in the symbol tables), and the `fix/api-quirks-2` fixes
(level 6: interrupts leave nothing armed, the pc at a park, `mds_write` for palette/VRAM/OAM,
packets that cannot block, errors for unknown ids, Lua hooks in the point list, GDB reset events), and
`feature/probes` (level 7: function probes, the memory access log, breakpoint actions and overlay tags, none of
which parks the console; [section 5b](#5b-probes-the-memory-log-breakpoint-actions-and-overlay-tags)),
render control from `feature/render-control` (level 8: skipping 2D / 3D drawing, threaded 3D), and
`fix/savestate-irq` (level 9: a state load recomputes the CPU state it does not carry, so nothing of
the run before the load survives it).
See [Version and features](#version-and-features) for what changed at each level.

Contents

1. [Overview](#1-overview)
2. [Lifecycle and running](#2-lifecycle-and-running)
3. [Input](#3-input)
4. [Memory and CPU](#4-memory-and-cpu)
5. [Debugging](#5-debugging)
5b. [Probes, the memory log, breakpoint actions and overlay tags](#5b-probes-the-memory-log-breakpoint-actions-and-overlay-tags)
6. [Savestates](#6-savestates)
6a. [Action Replay cheat codes](#6a-action-replay-cheat-codes)
7. [Lua](#7-lua)
8. [GDB stub and socket server](#8-gdb-stub-and-socket-server)
9. [Python equivalents](#9-python-equivalents)
10. [Recipes](#10-recipes)
11. [Known issues and surprises](#11-known-issues-and-surprises)

---

## 1. Overview

### What the library is

melonDS without a window. One process can hold up to 16 emulated DS consoles. Each has its own
emulator thread, its own debugger (breakpoints, watchpoints including DMA writes, stepping, traces,
call stacks, call counts, coverage, last writer, wireless packet capture), its own Lua state and
its own input queue. The library also contains a socket server (`mds_serve`) that exposes the same
consoles over TCP. JIT is always off in the headless build (`Instance.cpp`: `args.JIT =
std::nullopt`); the interpreter runs every console.

### Consoles and ids

`mds_create` returns a console id, the lowest free slot in 0-15 ("too many consoles" beyond 16).
Ids are reused after `mds_destroy`. Every other per-console call takes that id; an unknown id fails
with `no console N`.

The id is also the console's slot in the in-process wireless link. A separate *effective id*
(`id + mds_instance_base()`) is used only for the firmware MAC address, the default save-file name
and Lua's `emu.getconsoleid()` (see [section 8](#8-gdb-stub-and-socket-server)).

### Threads and the command queue

Each console owns one emulator thread. Nothing outside that thread touches the console's `NDS`
object mid-frame: calls that need the console are posted to the console's *command queue* and run
by the emulator thread, while the calling thread waits for the result (`Instance::Call`). The
emulator thread drains the queue:

- at every **frame boundary** while running (`Instance::ThreadMain`),
- **at once** when the console is idle (the thread wakes for the command),
- **at once** while the console is parked at a break (`Instance::Park` drains while parked),
- while the console is blocked at a frame boundary **waiting for input** or at an input barrier.

So a call such as `mds_read` on a running console returns at the end of the current frame, and
what it reads is a frame-boundary state, never a mid-frame one (test
`test_reads_while_running_only_see_frame_boundary_states`). `mds_write` on a running console is
applied at the next frame boundary (test `test_write_while_running_applies_at_frame_boundary`).
Calls from several threads at once are safe (test `test_concurrent_calls_while_running`).

The calls fall into three groups by what they need:

| Needs | Calls |
| --- | --- |
| no console | `mds_version`, `mds_has`, `mds_error`, `mds_set_local_dir`, `mds_set_log_level`, `mds_set_pacing`, `mds_mp_timeout` (after the first `mds_create`), `mds_serve*`, `mds_set_instance_base` (no console may exist), `mds_instance_base` |
| any state | `mds_destroy`, `mds_stop_after`, `mds_interrupt` (debugger), `mds_state`, `mds_take_break`, `mds_frames`, `mds_reset_count`, `mds_wait`, `mds_screen`, `mds_set_keys`, `mds_touch`, `mds_set_lid`, all `mds_input_*`, `mds_debug_recv_pending`, `mds_read`, `mds_write`, all `mds_cheat_*`, `mds_savestate`/`mds_loadstate` (accepted, but fail unless idle), `mds_lua_output`, `mds_packets`, `mds_packets_take`, `mds_debug_packet`, `mds_mac`, `mds_save_path` |
| stopped (idle **or** parked; fails with `console N is running`) | `mds_set_rtc`, `mds_get_rtc`, `mds_run`, `mds_start`, `mds_get_regs`, `mds_set_reg`, `mds_lua_run`, `mds_lua_exec`, `mds_lua_stop`, and the debugger calls: `mds_gdb`, `mds_gdb_host`, `mds_bp_add`, `mds_wp_add`, `mds_point_remove`, `mds_point_enable`, `mds_point_list`, `mds_point_count`, `mds_step`, `mds_trace`, `mds_trace_take`, `mds_calls`, `mds_callstack`, `mds_threadstacks`, `mds_callcounts_take`, `mds_coverage`, `mds_coverage_take`, `mds_lastwriter`, `mds_lastwriter_get`, `mds_lastwriter_query`, and every call of level 7 (`mds_probe_*`, `mds_memlog_*`, `mds_action_*`, `mds_overlay_tags`, `mds_overlay_at`, `mds_callcounts_take_ov`, `mds_coverage_take_ov`); `mds_overlays` (no debugger needed) |
| idle only | `mds_load_rom` ("loading a ROM needs an idle console"), `mds_reset` ("reset needs an idle console"), `mds_savestate`, `mds_loadstate` ("savestates need a frame boundary; finish the frame first") |

The calls marked *debugger* above (`mds_interrupt` and the debugger calls) also need a console created
with `debug != 0`; on one created with `debug = 0` they fail with `console N was created without the
debugger (debug = 0)` (see [section 2](#creating-and-destroying)).

### Which calls block

- **Command-queue calls** (everything that reads or changes the console: `mds_read`, `mds_write`,
  `mds_screen`, `mds_get_regs`, the `mds_cheat_*` calls, the debugger calls, `mds_load_rom`, `mds_reset`, the savestate
  calls, `mds_mac`, the RTC calls, `mds_gdb`, and `mds_lua_run`/`mds_lua_exec`/`mds_lua_stop`)
  block until the emulator thread has run them: immediately when idle or parked, up to one frame
  when running. If the emulator thread is itself blocked (inside
  the GDB stub, or in a wireless receive waiting up to the MP timeout) they wait for that too
  *(inferred)*.
- **`mds_run`** blocks until every console it started has stopped, or the timeout passes.
  **`mds_wait`** blocks the same way without starting anything.
- **`mds_destroy`** joins the emulator thread: it waits for the current frame (or wait/park) to
  end.
- **Non-blocking:** `mds_start`, `mds_stop_after`, `mds_interrupt`, `mds_state`, `mds_frames`,
  `mds_reset_count`, `mds_take_break`, the input setters and the `mds_input_*` calls (they only lock the input queue),
  `mds_lua_output`, the packet calls, `mds_save_path`.
- The Lua calls run the script on the emulator thread, so they are serialised with every other
  caller's commands (the socket server's included); a script body runs there until its first
  `emu.frameadvance()` before the call returns. (API level 1 ran them on the calling thread.)

### Return values and errors

- Functions returning `int` return **-1 on failure** and set the error message. On success they
  return 0, or a count, an id, a queue depth, a length, as documented per function.
- `mds_run` and `mds_wait` return 0 (all stopped), 1 (timeout) or -1.
- `mds_take_break` returns 1 (an event was copied), 0 (none) or -1.
- Exceptions to the -1 rule: `mds_frames` returns `MDS_FRAMES_ERROR` (`UINT64_MAX`) for an unknown
  id (level 6; it returned 0, which a fresh console also returns); the legacy
  `mds_lastwriter_get` returns 0 for every error (the same value as "no record"; use
  `mds_lastwriter_query`); `mds_has` returns 0 or 1; `mds_serve_stop`, `mds_serve_name` and the
  setters `mds_set_local_dir`, `mds_set_log_level`, `mds_set_pacing` return nothing. `mds_destroy`
  returns 0 or -1 from level 6 (it returned nothing and ignored unknown ids).
- Buffer sizes and counts (`max`, `max_packets`, `max_data`, `mds_run`/`mds_wait`'s `count`) must
  not be negative: a negative one fails with `max must not be negative` (or the parameter's name)
  before anything is copied. 0 is allowed and copies nothing.
- **`mds_error()`** returns the message of the most recent failure **on the calling thread**: it is
  a `thread_local std::string` in `Api.cpp`. Errors raised on the emulator thread are copied back
  to the caller's thread. A successful call does not clear it, so read it only right after a -1.
  The pointer stays valid until the next failure on the same thread.
- Messages are plain text. Input errors start with a protocol code (`bad_payload: ...`,
  `input_queue_full`) because the socket server maps them to error codes.

### Library-wide settings

| Function | Does |
| --- | --- |
| `int mds_version(void)` | The API level: 7 for this branch (`MDS_API_VERSION` in `melonds_api.h`), 1 for every library before the level scheme. See [Version and features](#version-and-features). |
| `int mds_has(const char* name)` | 1 if the library has the named feature or exports the named function, else 0 (also for NULL). Level 2 and later; on a level-1 library look the symbol up. |
| `void mds_set_local_dir(const char* dir)` | Directory for the files the melonDS core opens *by relative name* through `Platform::OpenLocalFile` (DSi NAND, SD/FAT image index, OpenGL shader cache). NULL means `.`. It does **not** affect BIOS, firmware, ROM, save or state paths: those are opened exactly as given, relative to the process's working directory. Process-wide, not synchronised: set it before creating consoles. |
| `void mds_set_log_level(int level)` | Core log threshold: 0 Debug, 1 Info, 2 Warn (default), 3 Error. Messages at or above it go to stderr. Process-wide. |
| `void mds_set_pacing(int fps)` | 0 (default, also any value <= 0): run as fast as possible. Otherwise each console thread sleeps so that it runs at most `fps` frames per second; a console that falls behind does not catch up (the schedule restarts from now). Process-wide, read every frame. |
| `int mds_mp_timeout(int ms)` | Receive timeout of the in-process wireless link, in ms (default 25). `ms >= 0` sets it; -1 only reads it. Returns the current value. Fails with "create a console first" until the first `mds_create` of the process. |

### Version and features

`mds_version()` is the API level. It goes up whenever functions are added or a behaviour changes;
`mds_has(name)` tells about one feature. Clients that must also run against older libraries should
look `mds_has` up as a symbol first (it does not exist at level 1), then ask it. The Python wrapper
does this: `native.api_version()`, `native.has(name)` (see [section 9](#9-python-equivalents)).

| level | branch | changes |
| --- | --- | --- |
| 1 | up to the IPC merge (`merge/ipc`) | everything in the C API before this table existed; `mds_version` returned 1 throughout, as functions were added |
| 2 | `fix/api-quirks` | `mds_has`, `mds_serve_host`, `mds_gdb_host`, `mds_lastwriter_query`; the behaviour changes named by the features below |
| 3 | `feature/cheats` | `mds_cheat_add`, `mds_cheat_remove`, `mds_cheat_enable`, `mds_cheat_clear`, `mds_cheat_list`, `mds_cheat_get` (feature `cheats`); a state load or ROM load clears the console's codes |
| 4 | `merge/upstream-debug` | `mds_threadstacks`; the call tracker, PC changes at a break, pipeline refetch, Lua `memory.execute`, `debugger.threadstacks` and `wifi.*`, ROM overlays for the server's symbols (the features of level 4 below) |
| 5 | `merge/upstream-debug-2` | `mds_overlays`; the GDB stub's per-connection reset, EOF detection, extended mode and `monitor overlays`; ROM overlay detection in the core; DWARF line info in the symbol tables (the features of level 5 below) |
| 6 | `fix/api-quirks-2` | `mds_point_count`, `mds_reset_count`, `mds_debug_packet` (test hook); `mds_destroy` returns int; `mds_packet.total`; `MDS_POINT_LUA`, `MDS_BREAK_RESET`, `MDS_FRAMES_ERROR`; the behaviour changes named by the features of level 6 below |
| 7 | `feature/probes` | `melonds_probes.h`: `mds_probe_*`, `mds_memlog_*`, `mds_action_*`, `mds_overlay_tags`, `mds_overlay_at`, `mds_callcounts_take_ov`, `mds_coverage_take_ov` (the features of level 7 below); no existing call or output changed |
| 8 | `feature/render-control` | `mds_set_render`, `mds_set_threaded_3d`, `mds_render_stats`, `MDS_RENDER_SKIP_2D`, `MDS_RENDER_SKIP_3D`, `MDS_RENDER_STATS` (features of level 8 below); the soft renderer's threaded mode no longer redraws the current 3D frame when it is switched on. Built at level 7 in parallel with `feature/probes`, renumbered 8 at the merge: ask `mds_has` for the features, not the level |
| 9 | `fix/savestate-irq` | no new functions; savestate loads recompute derived CPU state, GX FIFO padding is zeroed (the features of level 9 below). Built at level 7 in parallel, renumbered 9 at the merge |

Features of level 2 (`mds_has` also answers 1 for every exported function name, `"mds_serve_host"`,
`"mds_read"`, ...):

| feature | meaning |
| --- | --- |
| `api_version` | `mds_version` reports the API level; `mds_has` exists |
| `debug_off_errors` | debugger calls fail on a console created with `debug = 0` ([section 2](#creating-and-destroying)) |
| `count_checks` | negative `max`/`count` arguments fail |
| `load_rom_idle` | `mds_load_rom` needs an idle console |
| `lua_queue` | the Lua calls run on the emulator thread; a first-step error fails `mds_lua_run` ([section 7](#7-lua)) |
| `step_exact` | `mds_step(n)` executes n instructions from idle and from parked ([Stepping](#stepping-mds_step)) |
| `dma_break` | DMA watchpoint breaks park at the DMA, `pc` = the DMA tag ([Breaks](#breaks-what-happens-and-mds_take_break)) |
| `lastwriter_query` | `mds_lastwriter_query` ([Last writer](#last-writer)) |
| `savestate_input` | savestates 14.2 carry `KeyInput`, the pen and the held input; a load restores them ([section 6](#6-savestates)) |
| `savestate_debug_reset` | a state load clears call stacks, a pending watchpoint hit and the last-writer records |
| `mic_noise_emulated` | level-mic noise is seeded from the emulated frame, which states carry |
| `serve_host` | `mds_serve_host`, `melonds_server --host` ([section 8](#8-gdb-stub-and-socket-server)) |
| `gdb_host` | `mds_gdb_host`; the GDB stub listens on 127.0.0.1 by default |

Feature of level 3:

| feature | meaning |
| --- | --- |
| `cheats` | the `mds_cheat_*` calls: Action Replay codes run by the core's engine every frame ([section 6a](#6a-action-replay-cheat-codes)) |

Features of level 4:

| feature | meaning |
| --- | --- |
| `thread_stacks` | `mds_threadstacks` and Lua `debugger.threadstacks`: the stacks of the threads that are not running ([Call tracking](#call-tracking-call-stacks-and-call-counts)) |
| `callstack_unwind` | the call tracker also follows calls made without `BL`, tail calls and NitroSDK context switches, and seeds the frames below the first call from stack memory; call counts include those calls ([Call tracking](#call-tracking-call-stacks-and-call-counts)) |
| `step_redirect` | a PC changed at a break (`mds_set_reg` 15) or by a hook takes effect before anything executes: the hooks run again at the new PC (a breakpoint there fires) and `mds_step(n)` counts n instructions from it ([Stepping](#stepping-mds_step)) |
| `pipeline_refetch` | a write made at a break or from a hook (`mds_write`, Lua `memory.write*`) over the instruction about to run, or the one after it, takes effect: the CPU refetches its pipeline ([Breakpoints and watchpoints](#breakpoints-and-watchpoints)) |
| `lua_execute` | Lua `memory.execute(op, ...)` in exec hooks and at breaks ([section 7](#7-lua)) |
| `lua_wifi` | Lua `wifi.hook`, `wifi.inject`, `wifi.decode` ([section 7](#7-lua)) |
| `symbols_rom` | the socket server's `symbols_load` takes overlay images from the running ROM (no extract needed) and reads the config's `delinks.txt` ([section 8](#8-gdb-stub-and-socket-server)) |

Features of level 5:

| feature | meaning |
| --- | --- |
| `rom_overlays` | `mds_overlays`: the cart's ARM9 overlays that are resident now ([Resident overlays](#resident-overlays)) |
| `gdb_monitor_overlays` | the GDB stub answers `monitor overlays` with the resident overlays ([section 8](#8-gdb-stub-and-socket-server)) |
| `gdb_session_reset` | a GDB disconnect, including the client closing the socket (EOF, now detected), clears the stub's breakpoints and watchpoints, no-ack mode and extended mode, so the next client starts clean |
| `gdb_extended` | GDB extended mode (`!`, `target extended-remote`): `vKill` keeps the connection, `vRun` restarts the console, and a GDB restart (`monitor reset`, `vRun`) keeps a direct-booted ROM direct-booted |

Features of level 6 (the fixes are listed with what changed in
[Fixed in fix/api-quirks-2](#fixed-in-fixapi-quirks-2)):

| feature | meaning |
| --- | --- |
| `interrupt_clean` | a park and the end of a run drop the break requests left over (the other CPU's half of `mds_interrupt`, and those of a group's consoles `mds_wait` interrupted); `mds_interrupt` ignores a stopped console ([Running](#running)) |
| `pc_next` | `mds_get_regs` index 15 is each CPU's next instruction at every park, for both CPUs; a request served at a frame boundary reports the ARM9's next instruction ([Registers](#registers)) |
| `write_regions` | `mds_write` and Lua `memory.write*` write palette, VRAM and OAM with halfwords on the ARM9, read every write back, and fail naming the region where they cannot write ([Reading and writing memory](#reading-and-writing-memory)) |
| `packets_truncate` | `mds_packets_take` cuts a first packet longer than `max_data` short (`mds_packet.total` > `length`) instead of returning 0 forever ([Wireless packet capture](#wireless-packet-capture)) |
| `unknown_id_errors` | `mds_frames` returns `MDS_FRAMES_ERROR` and `mds_destroy` -1 for an unknown id |
| `point_lua_tag` | `mds_point_list` sets `MDS_POINT_LUA` in `flags` for Lua hooks; `mds_point_remove` refuses them ([Breakpoints and watchpoints](#breakpoints-and-watchpoints)) |
| `point_count` | `mds_point_count`: how many points `mds_point_list` has to list |
| `gdb_reset_event` | a GDB restart (`vRun`, `R`, `r`, `monitor reset`) queues an `MDS_BREAK_RESET` event, which the socket server forwards; `mds_reset_count` ([Breaks](#breaks-what-happens-and-mds_take_break), [section 8](#gdb-stub)) |

Features of level 7 (`feature/probes`; all four in [section 5b](#5b-probes-the-memory-log-breakpoint-actions-and-overlay-tags)).
They only add functions: a client that does not call them sees no change. Clients that must also work with libraries
of other branches should ask `mds_has` for these names rather than compare levels:

| feature | meaning |
| --- | --- |
| `probes` | `mds_probe_add`, `_remove`, `_list`, `_capacity`, `_take`, `_stats`: every call of a function with r0-r3, memory behind registers, LR, SP, depth and overlay at entry, and r0, r1 at its return, without parking |
| `memlog` | `mds_memlog_add`, `_remove`, `_list`, `_capacity`, `_take`, `_stats`: every CPU (and DMA) read and write in ranges, without parking |
| `bp_actions` | `mds_action_add`, `_remove`, `_list`, `_take`, `_stats`: return early, set a register, write memory or skip instructions at an address, without parking; every application logged and counted |
| `overlay_tags` | `mds_overlay_tags`, `mds_overlay_at`, `mds_callcounts_take_ov`, `mds_coverage_take_ov`, and `mds_probe_record.overlay`: the overlay of ARM9 code judged from memory; call counts and coverage per (address, overlay) |

Features of level 8 ([Render control](#render-control)):

| feature | meaning |
| --- | --- |
| `render_skip` | `mds_set_render`: skip 2D drawing and/or 3D rasterising from the next frame boundary; display capture still gets what it needs, so emulated state never changes |
| `threaded_3d` | `mds_set_threaded_3d`: the soft renderer draws 3D on a thread of its own (same pixels) |
| `render_stats` | `mds_render_stats`: the settings asked for and in force, and the skip counters |

Features of level 9 (core changes, so they apply to every frontend built from this branch):

| feature | meaning |
| --- | --- |
| `savestate_derived` | a state load recomputes the CPUs' pending-interrupt lines and the ARM9 code-fetch timing from the loaded state, instead of keeping their pre-load values: a console that loads a state runs exactly as a fresh console that loads it, and as the console that saved it ran on ([section 6](#6-savestates)) |
| `gx_fifo_zeroed` | GX command FIFO entries are written with their 3 padding bytes zeroed, so states (which hold the FIFO arrays raw) no longer contain stray stack bytes ([section 6](#6-savestates)) |

### Wireless between consoles, and one console per process

The first `mds_create` installs melonDS's *LocalMP* interface: every console in the process is on
one emulated local-wireless link (and one shared Slirp network for Wi-Fi). Consoles in different
processes are never linked. Local multiplayer therefore needs all consoles in one process, run
together (`mds_run` with several ids, or `mds_start` on each), and a blocking wireless receive
waits up to `mds_mp_timeout` ms for a partner.

For deterministic work (scenario replays, contrast pairs, coverage diffs) keep **one console per
process**. The dq9-re harness found that a second console in the same process changes a game's run
even while it is idle (different RAM hash for DQ9; see the dq9-re playbook, "Lessons"). The
mechanism is not pinned down in the code *(inferred: the shared LocalMP state and its receive
timeouts)*; the rule is empirical.

---

## 2. Lifecycle and running

### Creating and destroying

```c
int  mds_create(const char* bios9, const char* bios7, const char* firmware, const char* nickname, int debug);
int  mds_destroy(int id);   /* void before level 6 */
```

- `bios9`, `bios7`: paths to BIOS dumps, or NULL/"" for melonDS's built-in FreeBIOS. A file must be
  exactly the BIOS size (4 KiB ARM9, 16 KiB ARM7) or creation fails with `bad BIOS image: PATH`.
- `firmware`: a firmware dump, or NULL/"" for generated firmware. Errors: `cannot open firmware`,
  `bad firmware image`.
- `nickname`: if not empty, the first 10 bytes become the firmware user nickname (each byte taken
  as one UTF-16 code unit).
- `debug`: nonzero builds the CPUs in the debug interpreter (`InterpreterGDB` mode), which calls the
  debugger hooks before every instruction and on every data access. **A console created with
  `debug = 0` has no debugger**: the hooks never run, so every call that would silently do nothing
  fails instead, with `console N was created without the debugger (debug = 0)`: `mds_bp_add`,
  `mds_wp_add`, `mds_point_remove`, `mds_point_enable`, `mds_point_list`, `mds_point_count`, `mds_step`, `mds_trace`,
  `mds_trace_take`, `mds_calls`, `mds_callstack`, `mds_threadstacks`, `mds_callcounts_take`, `mds_coverage`,
  `mds_coverage_take`, `mds_lastwriter`, `mds_lastwriter_query` (and `mds_lastwriter_get`, which
  returns 0), `mds_interrupt`, `mds_gdb`/`mds_gdb_host`, and in Lua the `event.onmemory*` hooks and
  the `debugger.*` functions except `peek` and `resume` (a Lua error with the same text). The socket
  server answers these with `not_supported`. Everything else (running, input, memory, registers,
  savestates, Lua without hooks, packet capture) works. Use `debug = 0` only for speed. The Python
  wrapper defaults to `debug=True`, as do the dq9-re harness and `melonds_server`.
- The new console is reset but **not started**: until `mds_load_rom` or `mds_reset`, running it
  emulates nothing (tests' `base.py`: "run() does nothing until the first reset()"). A console
  without a cart can be reset and run; FreeBIOS then spins in BIOS code.
- The firmware MAC is changed for effective ids above 0 (see [section 8](#8-gdb-stub-and-socket-server)).

`mds_destroy` stops the console (the frame in progress finishes), joins its thread, fails every
command still queued for it ("console is shutting down"), releases barrier partners waiting for it
and removes it from the wireless link. It returns 0, or -1 with `no console N` for an unknown id
(level 6; before, it returned nothing and ignored unknown ids, so callers built against older
headers simply ignore the value). Do not destroy a console while another thread is still calling
into it: the registry lookup does not keep it alive.

Consoles that are never destroyed do not hang process exit (the registry is never destructed; test
`test_exit.py`), but closing them is cleaner.

### Loading a ROM and the save file

```c
int mds_load_rom(int id, const char* rom, const char* save);
int mds_save_path(int id, char* out, int max);
```

`mds_load_rom` needs an **idle** console, like `mds_reset` (a parked one fails with "loading a ROM
needs an idle console"). It reads the ROM, resolves the save path, inserts the cart and resets the
console (errors: `cannot read ROM: P`, `cannot parse ROM: P`).

- `save` NULL or "" selects the default: the ROM path with its extension replaced by `.sav` for
  effective id 0, and `.sav.N+1` for effective id N > 0 (`game.sav`, `game.sav.2`, ...; test
  `test_save_numbering`).
- If the save file exists, its contents become the cart's save memory. If not, the cart starts
  with blank save memory and the file is created on the first write.
- **The save file is live.** Every completed save write by the game (each SPI write sequence, on
  chip-select release) rewrites the whole file at once (`Platform::WriteNDSSave` ->
  `Instance::WriteSave`). **`mds_loadstate` also rewrites it** with the save memory stored in the
  state (`CartRetail::DoSavestate`). That is deliberate and kept for now (see
  [section 6](#6-savestates)): the cart's save memory is part of the state, and the file follows
  it. It matters because a save booted in place is overwritten by the first state load (with the
  save memory of whatever run made the state). **Boot a copy**: give every console its own copy
  and never point it at a save you want to keep (the dq9-re harness copies every save into a temp
  folder before booting it).
- Boot: with FreeBIOS or generated firmware the cart is direct-booted. With real BIOS and a
  bootable firmware dump, melonDS's `NeedsDirectBoot` is false and the console boots through the
  firmware *(inferred)*.
- Breakpoints, watchpoints, coverage and call counts survive a ROM load (they are not part of the
  reset; see the table in [section 5](#5-debugging)).

`mds_save_path` copies the path the last `mds_load_rom` resolved to (with its NUL) and returns its
length; "" (length 0) before any ROM was loaded. Fails with "buffer too small" if `max <= length`.

### Reset and the clock

```c
int mds_reset(int id);
int mds_set_rtc(int id, int year, int month, int day, int hour, int minute, int second);
int mds_get_rtc(int id, int* out);   /* out[6]: year, month, day, hour, minute, second */
```

`mds_reset` needs an **idle** console (parked is refused). It power-cycles with the same cart and
the save memory currently in RAM (the save file is not reread), direct-boots if needed and starts
the console. It also sets the frame counter to 0, empties the input queue, the input log and the
held input state, resets the touch timeline, empties the microphone FIFO and drops Lua's pending
input. Debugger effects are listed in [section 5](#5-debugging).

`int mds_reset_count(int id)` (level 6) returns how many times the console has been reset: by
`mds_reset`, by `mds_load_rom`, and by a GDB client (`vRun`, `R`, `r`, `monitor reset`; see
[section 8](#gdb-stub)), or -1 for an unknown id. A GDB restart does not restart `mds_frames`, so
this counter (and the reset event, [Breaks](#breaks-what-happens-and-mds_take_break)) is how a
caller tells a restarted run from an uninterrupted one.

`mds_set_rtc` takes a full year 2000-2099 (else "RTC year must be 2000-2099"); other fields out of
range are clamped by the core (month 1-12, day valid for the month, else 1; hour/minute/second
else 0). The clock is emulated: it advances with emulated time, not wall time. It is not seeded
from the host: a new console reads 2000-01-01 00:00:00. A reset keeps the clock; a savestate
carries its own clock, so **set the clock after loading a state**, not before. Games that read the
clock only replay identically with the same clock.

### Running

```c
enum { MDS_IDLE = 0, MDS_RUNNING = 1, MDS_PARKED = 2 };

int      mds_run(const int* ids, int count, uint32_t frames, int timeout_ms, int* states);
int      mds_wait(const int* ids, int count, int timeout_ms, int* states);
int      mds_start(int id, uint32_t frames);
int      mds_stop_after(int id, uint32_t frames);
int      mds_interrupt(int id);
int      mds_state(int id);
int      mds_take_break(int id, mds_break* out);
uint64_t mds_frames(int id);
int      mds_reset_count(int id);   /* level 6 */
```

**States.** *Idle*: at a frame boundary, nothing running. *Running*: the emulator thread is running
frames (or is blocked at a frame boundary waiting for input or a barrier; `mds_state` still says
running, `mds_input_waiting` says waiting). *Parked*: stopped **inside** a frame at a debugger
break; the CPU is between two instructions.

**`mds_start(id, frames)`** starts a stopped console for `frames` frames and returns at once. There
is no "forever": `frames` is a budget of frame ends, and the Python wrapper's default 0xFFFFFFFF is
just very long. From idle, `frames = 0` stops again at once. From **parked** it resumes: the frame
that was interrupted finishes and counts as one of the `frames`, and the remaining budget from
before the break is discarded. `mds_start(id, 0)` and `mds_start(id, 1)` on a parked console both
finish the current frame and go idle *(verified for 0)*. Starting a running console fails
("console N is running"). Starting clears the pending break event, so call `mds_take_break` first
(GDB reset events are kept).

**`mds_run(ids, count, frames, timeout_ms, states)`** checks that every console is stopped (if one
is running it fails and starts none), starts each with `frames`, then behaves as `mds_wait`.

**`mds_wait(ids, count, timeout_ms, states)`** waits until none of the consoles is running.
`timeout_ms < 0` waits forever; 0 polls once. If one console parks while others are still running,
`mds_wait` interrupts the running ones (once per call), so a break in one console stops the group;
those consoles park too (break kind `request`; the requests they leave on their other CPU are
dropped at the park, so the next run of the group runs). Returns 0 when none is running, 1 on timeout (the
consoles **keep running**: stop them with `mds_stop_after` and wait again), -1 for an unknown id.
`states`, if not NULL, receives each console's `MDS_*` state.

**`mds_stop_after(id, frames)`** changes the remaining budget of a *running* console (ignored when
idle or parked). The frame in progress always finishes; `frames` counts frame ends from now
including that one, so 0 and 1 both stop at the end of the current frame. One difference: a
console blocked waiting for input stops without running a frame on 0, and runs one more frame
(when input arrives) on 1.

**`mds_interrupt(id)`** (needs the debugger) asks a running console to park as soon as possible.
It sets a break request on **both** CPUs; the first CPU to execute an instruction parks with kind
`request`, and the park drops the other CPU's request (level 6; it used to stay armed and park the
next resume again at once). A console blocked waiting for input (or at a barrier) parks at the
frame boundary instead, without running a frame (test `test_interrupt_parks_at_the_frame_boundary`).
On a stopped (idle or parked) console it does nothing and returns 0 (level 6; an interrupt sent to
an idle console used to park its next run at its first instruction), and a run that ends before the
request is served drops it. Every park drops the break requests of both CPUs, whoever made them
(`mds_interrupt`, `mds_wait`, Lua `debugger.pause`); armed step counts are kept. When a frame
boundary is good enough, prefer `mds_stop_after(id, 0)`.

**`mds_state(id)`** returns `MDS_IDLE`, `MDS_RUNNING`, `MDS_PARKED` or -1.

**`mds_frames(id)`** returns the number of frames completed since the last reset or ROM load
(`MDS_FRAMES_ERROR`, `UINT64_MAX`, with `no console N` for an unknown id; 0 before level 6). A GDB
restart does not reset it (see `mds_reset_count`). It is incremented at the end of each frame, so a parked console has not yet
counted the frame it is in. It is **not** part of savestates: loading a state leaves it alone
(test `test_save_modify_load_restores`). It counts the process's frames; the console's emulated
frame count (which seeds the level-mic noise) is a separate counter that savestates 14.2 carry.

### Breaks: what happens and `mds_take_break`

When a breakpoint, watchpoint, step, request or fault fires, the emulator thread parks inside the
CPU loop: state becomes `MDS_PARKED`, the break is recorded, and the thread serves commands until
resumed. Registers, memory, the call stack and the trace can be read while parked; points can be
added and removed. Resume with `mds_start` or `mds_run`.

```c
typedef struct {
    int32_t  kind;   /* 0 exec, 1 read, 2 write, 3 step, 4 request, 5 fault, 6 reset (MDS_BREAK_RESET) */
    int32_t  cpu;    /* 9 or 7 */
    int32_t  size;   /* access size 1/2/4 for read/write; 0 otherwise */
    int32_t  id;     /* point id for exec/read/write; 0 for step, request, fault */
    uint32_t pc;
    uint32_t addr;
    uint32_t value;
    int32_t  dma;    /* DMA channel 0-3 for a DMA write, else -1 */
} mds_break;
```

| kind | `pc` | `addr` | `value` | the CPU stopped... |
| --- | --- | --- | --- | --- |
| exec | the breakpoint address (instruction not executed yet) | = pc | 0 | before the instruction |
| read / write | the instruction that made the access | the accessed address (start of the access) | write: the value written; read: memory at `addr` read through the debug path | before the **next** instruction of that CPU (the access has completed) |
| write by DMA (`dma >= 0`) | the DMA tag `0xFFFFFFF0 \| channel << 1 \| arm7` (no instruction made the write) | the unit's destination | the unit written | **at once**, inside the DMA: the unit is written, the next one is not (the CPUs are between two instructions); only the transfer's first watched unit parks |
| step | the next instruction | = pc | 0 | before that instruction |
| request | the next instruction; for a request served at a frame boundary (waiting for input or at a barrier) `cpu` is 9 and `pc` the ARM9's next instruction (level 6; it was the raw ARM9 R15, one pipeline stage ahead) | = pc | 0 | before that instruction |
| fault | the faulting instruction (ARM9 MPU prefetch or data abort) | the data address, or pc | 0 | before the abort is taken |
| reset (level 6) | the stub CPU's next instruction after the reset | 0 | 1: a GDB client reset the whole console (`vRun`, `R`, `r`); 0: only the stub's CPU (`monitor reset`) | **not a park**: the console keeps running |

For a DMA break `cpu` is the CPU whose DMA controller made the write (9 for channels of the ARM9's
DMA, 7 for the ARM7's), `dma` the channel 0-3 and `size` the unit size (2 or 4). The tag is the
same encoding as the last-writer records, so `pc >= 0xFFFFFFF0` always means "a DMA, not an
instruction". **One break per transfer:** the first watched unit of a DMA transfer parks; resuming
finishes the transfer without parking again (its later watched units still count a hit and run Lua
write hooks, whose return value then cannot park), and then the CPU continues. The channel's next
transfer can break again (test `DmaWatchpoint.test_one_break_per_transfer` in `test_api_fixes.py`,
`OneBreakPerDmaTransfer` in `test_pr_review.py`). A DMA across a watched 0x100-byte range parks once,
not 64 times. A watch hit and its park are ordered around the write: the hook sees memory before the
unit is written, the park comes after it. **What pc means at a DMA break:** the event's `pc` is the DMA tag, never an
instruction; `mds_get_regs` index 15 is, for both CPUs, the instruction each runs next (both are
between two instructions while the DMA runs). For an immediate DMA that is the instruction after the
store that started it. The program that started the DMA is not recorded. (API level 1 queued DMA
hits for the CPU's next instruction and reported that CPU's last instruction as `pc`; up to level 5
`mds_get_regs` index 15 was the CPU's last hooked instruction there.)

`mds_take_break(id, out)` copies the oldest undelivered event and clears it; it returns 0 if there is
none. Only one **park** event is kept: if the console parked twice without a take, the first is lost,
and starting the console clears it. If several points match at once, the first one wins and the
others only count a hit. **Reset events** (level 6, kind 6) are queued separately, up to the newest
64, ahead of the park event: a GDB client reset the running console (see
[section 8](#gdb-stub)). Starting the console does not clear them. While a socket client is
connected, the server thread takes the break events itself (they go to the client, reset events
included), so in-process callers see 0.

### Stepping: `mds_step`

```c
int mds_step(int id, int cpu, uint32_t count);   /* cpu: 7 or 1 = ARM7, anything else = ARM9 */
```

`mds_step` does not run anything. It **arms** a countdown on one CPU; the next `mds_start` /
`mds_run` runs the console and parks it with a `step` break when the countdown reaches zero.
`mds_step(id, cpu, n)` then a run executes exactly `n` instructions of that CPU and parks before the
next one, from **idle** and from **parked** alike (tests `StepExact` in `test_api_fixes.py`):
`mds_step(id, 9, 1)` from idle executes one ARM9 instruction, then parks; parked at a breakpoint,
the instruction at the breakpoint is the first of the `n`. The same holds for the socket server's
`step` message and Lua's `debugger.step`, which use the same countdown. (API level 1 executed `n - 1`
from idle, and from any park that was not in that CPU's own instruction hook.)

**From inside a hook** the count matches the break the hook stands for. In an exec hook
(`event.onmemoryexecute`, a breakpoint callback) `debugger.step(n)` counts the hooked instruction as
the first of the `n`, as at a breakpoint break (it executed `n + 1` before). In a write or read hook
the accessing instruction is already running and is not counted: the step parks where `n` steps
from that watchpoint's break would (that break parks at the next instruction); in a DMA write hook,
as from a DMA break. Tests `StepFromAHook` in `test_pr_review.py`.

`count = 0` disarms. The other CPU keeps running while one is stepped; the whole console parks. If
the CPU is halted (waiting for an interrupt) the step completes when it wakes. Give the run a frame
budget large enough to reach the step (the socket server uses a cap); the frame count advances only
when a frame ends. The step break is reported through `mds_take_break` with kind 3.

**A new PC while parked** (level 4, feature `step_redirect`). Changing PC at a break (`mds_set_reg`
index 15, Lua `emu.setregister("pc", ...)`, or a hook whose `memory.execute` branched) takes effect
before anything executes: on resume the instruction hooks run again for the new PC, so a breakpoint
there fires (kind exec, without running anything) and an armed step counts from the new PC:
`mds_set_reg(id, 9, 15, X)` then `mds_step(id, 9, n)` executes n instructions starting at X (test
`StepRedirect` in `test_upstream_debug.py`). The repeat hook call does not advance the step
countdown, because the instruction of the first call never ran. Before level 4 the instruction
already fetched at the break address ran first.

**Smart stepping is not in the C API.** The Qt debugger has "Step to branch" (Ctrl+F10: run to the
next instruction that may write PC at the current call depth, stepping over calls) and "Run to taken
branch" (Ctrl+F11: run until a branch, call or return actually jumps; stop at its target), and the
Qt automation commands `stepbranch`/`steptaken` (see `DEBUGGER.md`). They use a separate mechanism
in the core (`Debugger::StepTo`) that does not touch the step countdown: `mds_step`, the socket
server's `step` and Lua's `debugger.step` keep the exact counts above. Any break cancels a pending
smart step; a smart step does not cancel an armed count.

---

## 3. Input

Every input source feeds one per-console FIFO of **payloads**, one payload per frame:
`mds_set_keys` / `mds_touch` / `mds_set_lid`, `mds_input_submit`, `mds_input_submit_multi`, Lua's
`joypad.set` / `input.NDSTapDown`, and the socket server. At the start of each frame the emulator
thread takes the front payload, or applies the empty-queue rule, then runs the frame.

### Keys

Button bits (12 bits; bits above 11 are an error in payloads and are masked off by `mds_set_keys`):

| bit | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| key | A | B | Select | Start | Right | Left | Up | Down | R | L | X | Y |

A set bit means **pressed**. Keys are written to the hardware every frame (`ApplyButtons`).
Savestates 14.2 carry `KeyInput` and the held input too (see [section 6](#6-savestates)).

### The setters

```c
int mds_set_keys(int id, uint32_t pressed);
int mds_touch(int id, int x, int y);      /* x or y < 0: pen up; clamped to 255 / 191 */
int mds_set_lid(int id, int closed);
```

They are conveniences over the queue (`InputQueue::Modify`): each builds a payload from the
*newest* state (the last queued payload, else the last applied one), changes one thing, and either
**amends** the newest queued payload if that is also a setter payload, or appends a new one. So:

- they apply at the next frame start, not immediately (test
  `test_setters_apply_at_the_next_frame_not_immediately`);
- keys and touch set before one frame land in the same frame (`test_keys_and_touch_before_a_frame_land_together`);
- each setter keeps the other state (setting keys does not lift the pen);
- a setter goes **behind** payloads already queued (`test_setter_goes_behind_queued_payloads`);
- setter payloads always carry one touch point at offset 0 (the pen state).

What happens after the setter payload is consumed depends on the empty-queue mode: in the default
repeat mode the state is held until changed (local sources never stall), in neutral mode buttons
and pen are released the next frame, in wait mode the console waits for the next payload.

### Queued payloads

```c
typedef struct { uint32_t offset; uint16_t x, y; uint8_t down; uint8_t pad[3]; } mds_touch_point;

typedef struct {
    uint32_t seq;          /* free for the caller; logged when flags bit 0 is set */
    uint16_t buttons;      /* 12 bits, pressed = 1 */
    uint8_t  lid;          /* 1 = closed */
    uint8_t  touch_count;  /* 0 = pen unchanged */
    uint8_t  mic_kind;     /* 0 none, 1 samples, 2 level */
    uint8_t  flags;        /* bit 0: treat as a socket client's payload */
    uint16_t mic_count;    /* samples in mic[] for kind 1 */
    uint16_t mic_level;    /* kind 2: noise amplitude 0-65535 */
    uint16_t pad2;
} mds_input;

int mds_input_submit(int id, const mds_input* in, const mds_touch_point* points, const int16_t* mic);
```

Returns the new queue depth, or -1 with `bad_payload: ...` or `input_queue_full`; nothing is queued
on failure. Validation (`InputQueue::Validate`): at most 16 touch points, offsets strictly
increasing and below 560190, x <= 255, y <= 191; at most 796 mic samples; `mic_kind <= 2`; no
button bits above 11; `points` / `mic` may only be NULL when their counts are 0. The queue holds
600 payloads (10 s at 60 fps).

- **Touch** (`touch_count > 0`): the points are a *sub-frame timeline*, see below. With
  `touch_count = 0` the pen stays as it was.
- **Mic**: kind 1 pushes `mic_count` samples; kind 2 generates noise of amplitude `mic_level`;
  kind 0 pushes nothing. See the mic FIFO below.
- **`flags` bit 0** marks the payload as coming from a socket client: its `seq` is then kept in the
  input log, and the repeat-mode stall timeout applies to it. Other bits are ignored.
- **`seq`** has no effect on emulation. In-process, it is only visible in the input log (and only
  for flag-bit-0 payloads; local payloads log 0). Over the socket, the frame event that applies a
  payload echoes its seq (`FrameEvent::AppliedSeq`, `Protocol.h`).

### Empty-queue modes and queue control

```c
int mds_input_mode(int id, int mode, int stall_frames);  /* mode: 0 repeat, 1 neutral, 2 wait, -1 unchanged;
                                                            stall_frames: -1 unchanged, 0 never */
int mds_input_depth(int id);     /* payloads queued */
int mds_input_waiting(int id);   /* 1 while blocked at a frame boundary for input or a barrier */
int mds_input_clear(int id);     /* drop queued payloads */
```

What a frame gets when the queue is empty (`InputQueue::Next`):

- **repeat** (default): the last applied buttons, lid and pen. If the last consumed payload came
  from a socket client (flag bit 0) and `stall_frames > 0` (default 15), after `stall_frames` empty
  frames buttons and pen are released (the lid stays): a client that disappears cannot leave a key
  held (tests `test_client_payload_stalls_in_repeat_mode`, `test_local_payload_never_stalls`).
- **neutral**: buttons and pen released on every empty frame (lid kept).
- **wait**: the emulator thread blocks at the frame boundary without consuming frame budget until a
  payload arrives, the mode changes, the console is stopped (`mds_stop_after`) or interrupted
  (parks with `request`), or a command arrives (served, then it keeps waiting). Test class
  `WaitMode` in `test_input.py`.

Mic data is never repeated. A mode change applies from the next frame. The mode and stall timeout
survive reset and savestate loads. **`mds_serve` and every socket client connect/disconnect reset
the mode of every console to repeat with stall 15** (see [section 8](#8-gdb-stub-and-socket-server)).

`mds_input_clear` drops the queued payloads (leaving their barrier groups) and keeps the held state
and the log. `mds_reset` / `mds_load_rom` also clear the log and the held state. `mds_loadstate`
clears the queue and, for a 14.2 state, **restores the held state** the state was saved with (the
last applied buttons, lid and pen, whether it came from a socket client, and the empty frames since,
so the stall countdown continues): after a load the repeat rule repeats the loaded state's input,
not what was held before the load. A 14.2 state saved by another frontend (no headless section)
takes the held buttons, lid and pen from the loaded `KeyInput` and touch controller. A 14.1 or
older state leaves the held state as it was before the load (the level-1 behaviour). The mode, the
stall timeout and the log are never touched by a load.

### The input log

```c
typedef struct {
    uint64_t frame;        /* mds_frames() value of the frame the payload was applied to */
    uint32_t seq;          /* 0 for in-process sources */
    uint16_t buttons;
    uint8_t  lid;
    uint8_t  touch_count;
    uint8_t  mic_kind;
    uint8_t  pad;
    uint16_t mic_level;
    uint16_t pad2, pad3;
} mds_input_log_entry;

int mds_input_log(int id, uint64_t from_frame, mds_input_log_entry* out, int max);
```

Copies up to `max` entries with `frame >= from_frame`, oldest first, and returns how many. Only
**consumed payloads** are logged, one entry per payload: frames that ran on the empty-queue rule
(repeat/neutral) have no entry, so gaps in `frame` are normal. The log keeps the last 216,000
entries (an hour at 60 fps) and at most 32 MiB of touch/mic data. Reading does not clear it.
`frame` restarts at 0 after a reset and does not jump on a state load. In-process `seq` is 0 unless
the payload had flag bit 0. `touch_count` is the number of points actually applied, which is 1 for
a payload without points (the held pen), never 0. The touch points and mic samples are not exposed
by the C API (the socket's `input_log_read` returns full payloads).

### Barrier groups: `mds_input_submit_multi`

```c
int mds_input_submit_multi(const int* ids, int count, const mds_input* inputs,
                           const mds_touch_point* const* points, const int16_t* const* mic, int* depths);
```

Queues one payload for each console as one **barrier group**: all or none are queued (unknown
console, a bad payload, a console listed twice, or any full queue fails the whole call). When a
console reaches its group's payload at a frame boundary it waits (`mds_input_waiting` = 1) until
every member has reached its own payload, then all apply them and start the frame. `points[i]` and
`mic[i]` belong to `ids[i]`; the arrays or individual entries may be NULL when the counts are 0.
`depths[i]` receives each queue's depth. Returns 0 or -1.

A waiting member is released early when another member can no longer arrive: it was destroyed, its
queue was cleared or it was reset, or **it stopped running** (its frame budget ran out). A member
that stops is dropped from every group it belongs to, including groups whose payloads are still
queued, so later barriers do not wait for it either (test
`test_stopped_member_with_payload_left_does_not_block_later`). Barriers therefore only synchronise
consoles that run concurrently: start them with one long `mds_run`/`mds_start`, not one frame at a
time. A member is also released while a partner is blocked in a wireless receive (a parked console
cannot answer that partner's packets). A waiting console still serves commands, stops, and parks on
`mds_interrupt` (tests in `test_barrier.py`).

`mds_debug_recv_pending(id, pending)` is a test hook: it marks the console as blocked in (1) or out
of (0) a wireless receive, as the platform layer does around a blocking MP receive. Calls nest.

### Sub-frame touch timeline

Touch points carry `offset`, in system cycles (33.5 MHz) from the start of the frame; a frame is
560190 cycles, 263 scanlines of 2130 cycles, so scanline `n` starts at `n * 2130`. Each point holds
from its offset until the next point. Before the first point the pen keeps the state the previous
frame ended in; the last point carries into later frames until another payload has points. The core
consults the timeline whenever the ARM7 samples the touch controller, so a payload can press,
move and release within one frame (tests `test_touch.py`, `TouchTimelineTests.cpp`). The state at
offset 0 is also applied directly at frame start. A single point at offset 0 is the same as
`mds_touch` (`test_mds_touch_matches_a_single_offset_zero_payload`).

### Microphone FIFO

Each console has a sample FIFO (`MicFifo`) that the core reads at the touch controller's mic rate
(one sample per 704 system cycles, 795 or 796 per frame). It holds four frames (3184 samples) and
drops the oldest beyond that. When it is empty the core holds the last sample. Samples are only
accepted while the game has the microphone on (`Mic_Start`); samples sent while it is off are
dropped (`test_samples_sent_while_the_mic_is_off_never_arrive`), and turning it on or off empties
the FIFO. Kind 2 (`mic_level`) generates `795 or 796` samples of xorshift noise uniform in
`[-level/2, level/2]`, seeded from the console's **emulated frame** (frames since the last reset or
ROM load, `Instance::EmuFrame`), so level 0 is silence and the same game frame gives the same noise.
Without loads the emulated frame equals `mds_frames`; unlike it, savestates 14.2 carry it. A reset
empties the FIFO. A load of a 14.2 state puts back the samples the FIFO and the core's own mic
buffer held when the state was saved, so the same state and inputs give the same mic samples with or
without a load in between (tests `MicAcrossLoad`); a 14.1 or older state empties both and keeps the
emulated frame count from before the load (the level-1 behaviour, where the seed was `mds_frames`).

### Which applies when sources are mixed

There is one FIFO and one payload per frame. Payloads apply in the order they were queued, with
one exception: a setter call amends the newest payload when that payload is itself an unconsumed
setter payload. Lua's input is gathered while a script runs and submitted as one normal payload
when the script yields (or `mds_lua_exec` returns), behind whatever is queued (test
`test_joypad_set_lands_in_the_queue_after_queued_payloads`). When the queue is empty the mode
decides.

---

## 4. Memory and CPU

### Reading and writing memory

```c
int mds_read(int id, int cpu, uint32_t addr, uint32_t len, uint8_t* out);
int mds_write(int id, int cpu, uint32_t addr, uint32_t len, const uint8_t* data);
```

`cpu` 7 or 1 selects the **ARM7 bus**; any other value (9, 0, ...) the **ARM9 bus**. Both run on
the emulator thread (at the next frame boundary when running) and work in any state.

`mds_read` copies `len` bytes as that CPU sees them:

- ARM9: ITCM (below the ITCM size) and DTCM (at the DTCM base) come from the TCM arrays, so a DTCM
  mapped over main RAM is seen as the CPU sees it.
- Main RAM (`0x02xxxxxx`, mirrored) is read directly.
- **I/O (`0x04xxxxxx`) reads as 0**: registers are not read, so reads have no side effects and
  cannot show register values.
- Everything else goes through the core's bus read (`NDS::ARM9Read8` / `ARM7Read8`): shared and
  ARM7 WRAM, palette, VRAM (as currently mapped), OAM, GBA slot, BIOS. The ARM7 BIOS
  (`< 0x4000`) reads as 0xFF unless the ARM7 PC is inside the BIOS (the core's BIOS protection).

`mds_write` writes through `Debugger::WriteMemory` (level 6), as that CPU's bus takes it, and reads
every write back:

- Byte writes where the bus takes them: TCM, main RAM, shared and ARM7 WRAM, the ARM7's VRAM, GBA
  SRAM. Code written to RAM runs correctly (interpreter, no JIT cache).
- **ARM9 palette, VRAM and OAM (`0x05`-`0x07`)** ignore 8-bit writes on the bus (as on hardware),
  so there the call writes aligned **halfwords**; a byte at an odd start or end is merged with the
  byte beside it, which keeps its value (tests `WriteRegions` in `test_api_fixes2.py`). Up to level
  5 these writes were dropped while the call returned 0.
- **Refused, naming the region**, at the first byte that cannot be written, after writing the
  bytes before it (`mds_error`, for example `I/O at 0x04000208 cannot be written by the debugger
  (I/O writes have side effects)`):
  - **I/O** (`0x04xxxxxx`): never written, because register writes have side effects (they start
    DMAs, acknowledge interrupts, push FIFOs). It was already an error ("address not writable").
    Change I/O from guest code or Lua hooks running in the game instead.
  - **Read-only regions**: the GBA ROM area (`0x08000000`-`0x09FFFFFF`), the ARM9 BIOS
    (`0xFFFF0000`+) and the ARM7 BIOS (`0x00xxxxxx` on the ARM7) fail with `... is read-only`.
  - **A write that did not take** (the byte or halfword read back differs): VRAM with no bank
    mapped there, the palette or OAM of a 2D engine that is powered off (`POWCNT1`), GBA SRAM
    without a GBA cart, unmapped memory: `<region> at 0x... did not take the write (...)`.
- Writes do not trigger watchpoints and are not recorded by the last-writer table *(inferred: they
  bypass the CPU access hooks)*.
- Lua's `memory.write_*` and `memory.write_bytes_as_array` in the headless build write the same way
  and raise `memory write failed: <the same message>` ([section 7](#7-lua)). The Qt frontend's Lua
  and memory views are unchanged.

### Registers

```c
int mds_get_regs(int id, int cpu, uint32_t* out);   /* out[19]; console stopped */
int mds_set_reg(int id, int cpu, int reg, uint32_t value);
```

`out` layout:

| index | content |
| --- | --- |
| 0-14 | r0-r14 as seen in the CPU's **current mode** (melonDS keeps the active bank in `R[]`; other modes' banked r8-r14 are not exposed) |
| 15 | pc: the address of the instruction the CPU executes next, `R15 - 4` (ARM) or `- 2` (Thumb), idle and parked, for both CPUs; at a fault of that CPU, the faulting instruction |
| 16 | CPSR |
| 17 | SPSR of the current mode, 0 in User/System |
| 18 | raw R15 (between instructions: pc + 4 in ARM, pc + 2 in Thumb) |

Both CPUs are between two instructions when idle and at every park but a fault: an instruction
break parks before its instruction, a watchpoint before the accessing CPU's next one, a DMA break
between two DMA units (see [Breaks](#breaks-what-happens-and-mds_take_break) for what pc means
there), a request at a frame boundary or before an instruction. So index 15 is exact for the CPU
that broke and for the other one, and it follows a pc changed while parked (tests
`PcIsTheNextInstruction` in `test_api_fixes2.py`). A fault parks inside the faulting instruction:
there index 15 of the faulting CPU is the event's pc until the pc is changed *(inferred: the tests'
programs cannot raise an MPU fault)*. Up to level 5 index 15 was `Debugger::CurPC` while parked:
the last instruction the CPU's hook saw, one behind for the CPU that did not break, and 0 or stale
for a CPU without armed hooks.

`mds_set_reg` sets r0-r15 of the current mode (`register out of range` otherwise); CPSR and SPSR
cannot be set. `reg = 15` jumps (flushing the pipeline) to `value` and keeps the current ARM/Thumb
state: in Thumb state the address is forced odd, and an odd address in ARM state switches to Thumb.
Setting a register while parked at a breakpoint affects the instruction about to execute
*(inferred)*.

### Screen

```c
int mds_screen(int id, uint32_t* out);   /* 256 * 384 words */
```

Copies both screens of the last **completed** frame: the top screen (256x192) first, then the
bottom screen, as the player sees them (the core's screen swap is applied). Each pixel is a 32-bit
word `0xAARRGGBB` with alpha 0xFF, i.e. bytes B, G, R, A in memory (BGRA; PIL:
`Image.frombuffer("RGBA", (256, 384), buf, "raw", "BGRA", 0, 1)`). The 6-bit DS colours are
expanded to 8 bits. The frame buffer is swapped at the end of each frame, so after `mds_run(1)` the
screen shows that frame. Framebuffers are not in savestates: right after `mds_loadstate` the screen
still shows the frame from before the load, and 3D content lags a frame more; run two frames after
a load before trusting it (dq9-re harness experience). Works in any state; a running console
answers at the next frame boundary. Fails with "no framebuffer" if the renderer has none. With
drawing skipped ([Render control](#render-control)) the screen keeps the last frame drawn.

### Render control

```c
#define MDS_RENDER_SKIP_2D 1
#define MDS_RENDER_SKIP_3D 2
int mds_set_render(int id, uint32_t flags);
int mds_set_threaded_3d(int id, int on);
#define MDS_RENDER_STATS 9
int mds_render_stats(int id, uint64_t* out, int max);
```

API level 8 (features `render_skip`, `threaded_3d`, `render_stats`). Drawing is about 30% of a frame's
time in a 3D game (most of it 3D rasterising), and a headless run that only looks at the screen now and
then does not need it. Neither call changes emulated state: main RAM, VRAM, the registers and the whole
savestate are the same with any setting (tested: `tools/melonds-mcp/tests/test_render.py`; a full rebuild of
dq9-re's ~4000 scenarios reproduced every RAM and screen hash). Savestates do not carry either setting, and a
state load or a reset keeps them.

**`mds_set_render(id, flags)`** skips drawing from the next frame boundary on: a running console applies it
before its next frame, a stopped one before the frame it runs next, so a frame is never split. Unknown bits
fail ("unknown render flags N"); 0 draws everything again. Works in any state.

- `MDS_RENDER_SKIP_2D`: no BG/OBJ drawing, compositing or framebuffer writes (the VRAM caches the 2D renderer
  keeps are still updated, so the first frame drawn afterwards is exact). Sprites for line 0, which are drawn at
  VCount 262 of the frame before, are always drawn. `mds_screen` keeps showing the last frame drawn.
- `MDS_RENDER_SKIP_3D`: no 3D rasterising at VCount 215 (the texture caches are still updated); the next frame
  drawn redraws in full even when the 3D engine says nothing changed.

How many frames to draw before a screenshot: after clearing the flags, **2D is exact in the first frame drawn,
3D in the second** (a frame shows its own 2D lines, but the 3D frame drawn at VCount 215 of the frame before).
So clear both 2 frames before reading the screen (`native.RENDER_FRAMES_3D`). Measured on the synthetic
scene of `test_render.py` (2D and 3D colours that change every frame: with 3D, the first frame after skipping
still shows the old 3D) and on ten DQ9 states (field, town, battle, menu, a battle transition, a cutscene, a
map change, the title movie: 300-600 frames skipped, then 2 frames drawn give the reference screen, 1 frame
does not).

**Display capture.** Capture (DISPCAPCNT) writes the 2D engine A or 3D output into VRAM, so it is game state
and skipping must not change it. With capture on (VCount 0-191 of a frame whose DISPCAPCNT bit 31 was set),
every line is drawn in full even with `MDS_RENDER_SKIP_2D`, and its sprites the line before. The 3D frame a
capture needs is drawn at VCount 215 of the frame before; it is drawn although `MDS_RENDER_SKIP_3D` is set when
a capture is already requested then (bit 31 set) or the frame being drawn had capture on (games that capture
do so frame after frame). A capture requested later (between VCount 215 and the next frame) finds its 3D frame
skipped: it is drawn at the capture's first line instead (counted as a late 3D frame). That is exact unless
the game changed texture or texture palette VRAM after VCount 215, which melonDS's own renderer would not have
seen; the counter says whether it ever happened. DQ9 never enables capture in the states measured.

**`mds_set_threaded_3d(id, on)`** puts the soft 3D renderer on a thread of its own (melonDS's "threaded" 3D
setting; the headless library never set it, so 3D ran on the emulator thread). The pixels are the same; a
frame takes 15-20% less wall time in DQ9. A stopped console switches at once, a running one at its next frame
boundary. Switching is safe at any point and changes no pixel: the buffer already holds the current 3D frame,
so no frame is redrawn (the core used to start a redraw when the setting was switched on). One difference is
melonDS's own: a state loaded while threaded redraws the loaded 3D frame at once, so the first frame after
the load shows it, where the unthreaded renderer shows the stale buffer for that frame. Keep it off while
loading states if the first frame's screen must match an unthreaded run (the dq9-re harness turns it on only
while inputs play).

**`mds_render_stats(id, out, max)`** fills up to `max` values and returns `MDS_RENDER_STATS` (9; more than
`max` when cut short), on the emulator thread (a running console answers at its next frame boundary):

| index | value |
| --- | --- |
| 0 | skip flags asked for (`mds_set_render`) |
| 1 | skip flags in force (they change at the next frame boundary) |
| 2 | threaded 3D asked for |
| 3 | threaded 3D in force |
| 4 | visible lines whose 2D drawing was skipped |
| 5 | 3D frames not rasterised |
| 6 | lines drawn although 2D skipping was on, because capture was on |
| 7 | 3D frames drawn although 3D skipping was on, because a capture was likely |
| 8 | late 3D frames: a capture started without its 3D frame, which was then drawn at its first line |

The counters run from the console's creation (a reset keeps them). Python: `Console.set_render(skip_2d,
skip_3d)`, `set_render_flags(flags)`, `set_threaded_3d(on)`, `render_stats()` (a dict named by
`native.RENDER_STAT_NAMES`).

---

## 5. Debugging

Everything in this section except packet capture needs a console created with `debug != 0`; on
one created with `debug = 0` the calls fail with `console N was created without the debugger
(debug = 0)` (see [section 2](#creating-and-destroying)). Setup calls need a stopped console. `cpumask` arguments
use bit 0 = ARM9 and bit 1 = ARM7 (3 = both); `cpu` arguments use 7 or 1 for ARM7, anything else
for ARM9.

### Breakpoints and watchpoints

```c
int mds_bp_add(int id, uint32_t cpumask, uint32_t addr);
int mds_wp_add(int id, uint32_t cpumask, uint32_t addr, uint32_t size, uint32_t flags);  /* flags: 1 read, 2 write */
int mds_point_remove(int id, int point);
int mds_point_enable(int id, int point, int enabled);
int mds_point_list(int id, mds_point* out, int max);
int mds_point_count(int id);   /* level 6 */

#define MDS_POINT_LUA 0x100    /* level 6 */

typedef struct {
    int32_t  id;
    int32_t  watch;     /* 0 breakpoint, 1 watchpoint */
    uint32_t cpumask;
    uint32_t flags;     /* watchpoints: 1 read, 2 write; breakpoints: 0; | MDS_POINT_LUA for a Lua hook */
    uint32_t enabled;
    uint32_t start;
    uint32_t end;       /* inclusive; = start for breakpoints */
    uint64_t hits;
} mds_point;
```

- `mds_bp_add` returns the new point id. Bit 0 of `addr` is cleared, so a Thumb function address
  with its interworking bit works. The CPU parks before executing the instruction at that address.
- `mds_wp_add` watches `[addr, addr + size - 1]` (clamped at 0xFFFFFFFF) for reads (flag 1),
  writes (2) or both (3); only `flags & 3` is used. `size = 0` or no read/write flag fails with
  "bad watchpoint". An access matches if it overlaps the range. It covers CPU loads and stores
  (including TCM) and, for writes, **DMA**: an ARM9 DMA channel write checks the ARM9 watchpoints
  (`cpumask` bit 0), an ARM7 DMA write the ARM7 ones. DMA source reads and other bus masters are not
  watched. A CPU access breaks before the next instruction of that CPU; a DMA write breaks at once,
  between two units, with `pc` = the DMA tag, once per DMA transfer (see the break table in
  [section 2](#breaks-what-happens-and-mds_take_break)). Debugger reads and writes
  (`mds_read`/`mds_write`) do not trigger watchpoints.
- Ids come from one per-console counter starting at 1, shared by breakpoints, watchpoints and Lua
  memory hooks (and Lua `wifi.hook` ids); they are never reused. `mds_point_remove` /
  `mds_point_enable` fail with "no such point".
- `mds_point_list` copies up to `max` points (`max >= 0`), all breakpoints first then all
  watchpoints, each in creation order, and returns the count copied. **`mds_point_count`** (level
  6) returns how many there are, so a buffer of that size gets them all; `mds_point_list` keeps
  returning the number copied (callers that index their buffer with it stay safe), so `n ==
  max` alone no longer has to stand in for truncation.
- **Lua memory hooks** (`event.onmemoryexecute`/`onmemoryread`/`onmemorywrite`) are points with a
  callback. They appear in the list with `MDS_POINT_LUA` (0x100) set in `flags` (level 6; the
  watchpoint read/write bits are kept), and `mds_point_remove` refuses them, and wifi hook ids:
  `point N is a Lua hook: the script removes it (event.unregisterbyid / unregisterbyname)`. Up to
  level 5 they were plain points that `mds_point_remove` deleted, leaving the script with a dead id.
  `mds_point_enable` works on them. The socket server's `point_list` carries the same flags.
- `hits` counts every enabled match, including matches that did not park because another event
  was already pending or a Lua hook returned false. Disabled points are not matched.

Cost: with at least one enabled breakpoint, every instruction of that CPU calls the debugger hook
and checks a 1 MiB page table; with a watchpoint, every data access of that CPU does. Points
persist across reset, ROM loads and savestate loads.

**Writing code at a break** (level 4, feature `pipeline_refetch`). The CPU has already fetched the
instruction it is parked before and the one after it. A write made while parked at a break or from
inside a hook (`mds_write`, Lua `memory.write*`) that overlaps those 8 bytes (4 in Thumb) makes the
CPU refetch them, so the new code is what runs (test `PipelineRefetch`). Before level 4 the old
opcodes ran once more. Writes from an idle console (at a frame boundary) need no refetch, and a
write made by a watchpoint callback in the middle of an instruction does not refetch. The refetch
reads code through the bus like a fetch *(inferred: it may add code-fetch cycles, so a run that
writes code at a break is not cycle-identical to one that does not)*.

### Instruction trace

```c
int mds_trace(int id, uint32_t cpumask, uint32_t start, uint32_t end, uint32_t capacity);
int mds_trace_take(int id, mds_trace_entry* out, int max);

typedef struct {
    uint64_t cycle;  /* ARM7: ARM7 timestamp; ARM9: ARM9 timestamp >> clock shift (system cycles) */
    uint32_t pc;
    uint32_t instr;  /* the opcode fetched for pc */
    uint32_t cpsr;
    uint32_t cpu;    /* 9 or 7 */
} mds_trace_entry;
```

`mds_trace` (re)configures a ring buffer of `capacity` entries recording every instruction of the
masked CPUs whose pc is in `[start, end]` (inclusive). `capacity = 0` turns tracing off. Each call
discards everything recorded and staged. When the ring is full the oldest entries are overwritten.
An instruction is recorded just before it executes, so the instruction a break stops at is in the
trace although it has not run yet (test `TraceTests`). Entries of both CPUs are stored in
emulation order, which is not strictly cycle order across CPUs *(inferred: melonDS runs the CPUs
in alternating batches)*.

`mds_trace_take` returns up to `max` entries, oldest first, and removes them. It drains the whole
ring into a staging buffer on the first call; a call that stops at `max` leaves the rest staged for
the next call, so loop until it returns 0. Cost: one hook call per instruction of the traced CPUs,
24 bytes per entry. A reset empties the ring (not the staged part); a savestate load does not.

### Call tracking: call stacks and call counts

```c
int mds_calls(int id, int enabled);
int mds_callstack(int id, int cpu, mds_frame* out, int max);
int mds_callcounts_take(int id, int cpu, uint32_t* entries, uint64_t* counts, int max);

typedef struct {
    uint32_t site;       /* address of the call instruction */
    uint32_t entry;      /* call target (Thumb bit cleared) */
    uint32_t ret;        /* return address (LR, bit 0 cleared) */
    uint32_t sp;         /* SP right after the call */
    uint32_t mode;       /* CPSR mode bits at the call (0x10 usr, 0x12 irq, 0x13 svc, 0x1F sys, ...) */
    uint32_t exception;  /* 1: an exception entry, not a call */
} mds_frame;
```

`mds_calls(id, 1)` turns tracking on for both CPUs; `mds_calls(id, 0)` turns it off and **clears the
call stacks** (not the counts).

**What counts as a call** (level 4, feature `callstack_unwind`; the tracker came with the merge of
ZevyaDev's `debug` branch). A frame is pushed for:

- `BL`, `BLX` (immediate and register) in ARM state, and Thumb `BL`/`BLX` (the second half of the
  pair) and `BLX Rm` (the core's call hook);
- a jump to an address other than the next instruction and other than LR, made while LR holds the
  return address: the `MOV LR, PC; BX Rn` / `LDR PC, ...` idioms, and `LDR LR, =ret` followed by an
  indirect jump;
- a function entered without a tracked call whose first instruction is a `PUSH {..., LR}`
  (`STMDB SP!` / Thumb `PUSH`) and whose LR is not already the return of one of the 8 innermost
  frames: the entry is taken as the target of the last jump if that is at most 0x20 bytes before.

A tail call is followed instead of pushed: an indirect jump (or a jump to a known entry) with SP, LR and
mode unchanged retargets the innermost frame to the new entry and counts it. `B`/`BX` to the next
part of the same function push nothing. IRQs and SWIs are tracked as exception frames but not counted.
These are heuristics *(inferred)*: hand-written code that pushes LR without being called can add a
frame, and a call through a veneer is counted at the veneer.

**Call stack.** Each call pushes a frame (at most 4096 deep; the oldest frame is dropped beyond
that). Before every instruction the tracker pops frames whose return address has been reached in
the same mode with the stack back at or above the frame's SP, and frames whose SP the stack has
risen past (so tail calls and longjmp-style exits unwind). It is **thread aware** *(heuristic)*:
when SP jumps by 0x400 or more, or changes in the same mode without a stack-adjusting instruction,
the current stack is parked under the old SP and the parked stack whose SP is closest to the new SP
(within 0x200) is restored, keeping up to 64 parked thread stacks. A switch that happens on the way
out of an exception (NitroSDK loading another thread's context) rebuilds the resumed thread's
innermost frame from its LR, and if nothing was known about that thread, its stack is unwound from
stack memory. It is **interrupt aware**: an IRQ or SWI saves the interrupted stack (up to 16
nested) and starts a new one whose first frame is the exception entry (`exception = 1`, `site =
ret` = the interrupted address, `entry` = the vector address); the exception return restores it.
Unwinding only runs in debug mode. Stacks are tracked in system and user mode only: in an exception
mode SP changes are not treated as thread switches.

**Frames from before tracking.** After `mds_calls(id, 1)` and after a state load, the first frame
pushed outside an exception also **seeds the frames below it** from stack memory: the caller chain
is unwound by scanning back for each function's prologue (how much it pushed, where it saved LR)
for up to 64 frames. Seeded frames are not counted in the call counts. So a stack read soon after
enabling tracking is usually complete, not just the calls made since *(inferred; the unwinder relies
on standard prologues)*.

`mds_callstack` copies the stack of one CPU **innermost first**: the current frames, then for each
interrupted context its exception frame and the frames below it. It returns the count copied
(empty if tracking is off). A reset and a savestate load clear the stacks (a load puts the CPUs
somewhere else entirely; tracking stays on and builds new stacks from there, seeded as above).

```c
int mds_threadstacks(int id, int cpu, uint32_t* sps, uint32_t* depths, int max_stacks,
                     mds_frame* out, int max_frames);   /* level 4, "thread_stacks" */
```

`mds_threadstacks` returns the stacks of the threads that are **not running**: first the parked
stacks of the tracker (each under the SP it was parked at), then every other thread of the
NitroSDK thread list (the `OSThreadInfo` pointed to by `0x027FFFA0` for the ARM9, `0x027FFFA4` for
the ARM7; up to 64 threads) whose saved context was not already tracked, unwound from its saved
PC, SP and LR. A tracked thread's innermost frame is refreshed from its saved context. It returns
the number of stacks copied (at most `max_stacks`); `sps[i]` is the stack's SP and `depths[i]` its
number of frames; the frames of all stacks are packed into `out` one stack after the other,
innermost first, at most `max_frames` in total (a stack cut short has a smaller `depths[i]`). Both
counts must not be negative. It needs the debugger and a stopped console. Thread-list stacks are
found even with tracking off (they come from memory); the parked stacks need tracking. A game that
is not built on NitroSDK, or before `OS_InitThread`, has no thread list *(inferred)*. Lua:
`debugger.threadstacks([cpu])` returns `{ {sp = ..., frames = {...}}, ... }`. Python:
`Console.thread_stacks(cpu=9)`.

**Call counts.** Each pushed frame (above) and each followed tail call increments a counter for its
`entry`, per CPU; seeded frames do not. From level 4 this includes calls made without `BL`, so counts
can be higher than before for the same run.
`mds_callcounts_take` returns up to `max` (entry, count) pairs in no particular order and removes
them (staged like the trace: loop until 0). Counts accumulate across frames, resets and state
loads until taken; turning tracking off does not clear them. Python sums the chunks into a dict.

Cost: one hook call per instruction (unwinding) plus work per call; it also arms instruction hooks
for both CPUs.

### Coverage

```c
int mds_coverage(int id, int enabled);
int mds_coverage_take(int id, int cpu, uint32_t* out, int max);
```

Coverage records **every executed instruction address** (one bit per halfword, so ARM and Thumb
both resolve to exact instructions), per CPU. It is per instruction, not per block or function.
`mds_coverage_take` returns up to `max` addresses, sorted ascending and unique, and removes them
(staged: loop until 0). Turning coverage off stops recording but keeps what was recorded until
taken; reset and state loads keep it too.

Coverage catches code that call counts miss: code reached by `B`/`BX` (tail calls, jump tables,
switch cases, branches inside a function), code entered by `MOV PC`/`LDR PC`, interrupt handlers,
and which parts of a function ran. Call counts tell how often; coverage tells whether. Cost: a hook
call per instruction plus a hash lookup when the 4 KiB page changes; 256 bytes per touched page.

### Last writer

```c
int      mds_lastwriter(int id, int enabled);
uint32_t mds_lastwriter_get(int id, uint32_t addr);
```

`mds_lastwriter(id, 1)` allocates a 16 MiB table (one word per main-RAM word) and clears it;
`mds_lastwriter(id, 0)` frees it. While enabled, every write to **main RAM** (`0x02xxxxxx`, the
4 MiB mirror) records, for the 32-bit word containing it (so a byte write tags the whole word):

- a CPU store: the storing instruction's address with bit 0 set for ARM7 (`pc | 1`), clear for
  ARM9;
- a DMA write: `0xFFFFFFF0 | channel << 1 | cpu`, cpu bit 1 = ARM7.

```c
int mds_lastwriter_query(int id, uint32_t addr, uint32_t* raw, int* dma);
```

`mds_lastwriter_query` returns **1** with the record in `*raw` and the DMA channel in `*dma` (-1 for
an instruction), **0** for no record (never written since tracking was enabled, a reset or a state
load, or an address outside main RAM; `*raw = 0`, `*dma = -1`), or **-1** for an error: unknown
console, running console, no debugger, or "last-writer tracking is off (mds_lastwriter(id, 1))".
Either pointer may be NULL. Decode `raw`: `w >= 0xFFFFFFF0` DMA channel `(w >> 1) & 3` of CPU
`w & 1 ? 7 : 9` (the same tag a DMA watchpoint break reports as `pc`); else pc `w & ~1` of CPU
`w & 1 ? 7 : 9`. Tests: `WriterTests` (ARM9, ARM7, DMA), `LastWriterQuery`.

`mds_lastwriter_get` is the **legacy** form: it returns the raw record, and 0 both for "no record"
and for every error, which cannot be told apart. Lua's `debugger.lastwriter(addr)` returns
`pc, cpu` for an instruction, `nil, cpu, channel` for a DMA write and `nil` for no record.

A reset and a state load zero the table (the records described the RAM before the load). Cost: a
hook on every data access of both CPUs (it arms the data hooks) and 16 MiB.

### Resident overlays

```c
int mds_overlays(int id, int* ids, uint32_t* bases, uint32_t* sizes, int max);   /* level 5, "rom_overlays" */
```

The ARM9 overlays of the loaded cart that are in memory now: the same answer as the GDB stub's
`monitor overlays` and the overlay detection of the symbol tables (one implementation, in the core:
`src/debug/RomOverlays`). The overlay table and the overlay files are read from the ROM image
(BLZ-compressed ones decompressed), and an overlay counts as resident when at least 6 of 8 sampled
16-byte windows of its code match ARM9 memory (read without side effects). Overlays that share an
address are told apart that way.

It writes the ids, base addresses and sizes (code + bss) of up to `max` resident overlays in the
order of the ROM's overlay table (each array may be NULL) and returns how many are resident, which
may exceed `max`. Without a ROM it returns 0. It needs a stopped console (idle or parked), not the
debugger, and runs on the emulator thread; it reads and decompresses the overlay files on every call
(milliseconds for DQ9), so call it at a break or a frame boundary, not per instruction. Python:
`Console.overlays()` -> `[{"id", "base", "size"}]`. Tests: `test_gdb_overlays.py`.

### Wireless packet capture

```c
int mds_packets(int id, int enabled, int capacity);
int mds_packets_take(int id, mds_packet* headers, uint8_t* data, int max_packets, int max_data);

int mds_debug_packet(int id, int sent, int type, const uint8_t* data, int len);   /* level 6, test hook */

typedef struct {
    uint64_t timestamp;
    uint32_t sent;
    uint32_t type;
    uint32_t length;    /* bytes of the packet in data */
    uint32_t total;     /* level 6: the packet's size; > length when it was cut short */
} mds_packet;
```

`mds_packets` turns capture on or off for one console and empties the ring and the staging; the
ring holds `max(capacity, 1)` packets and drops the oldest. Logged: sent packets (`type` 0 data, 1
host command, 2 reply, 3 ack) and received packets (`type` 0 data, 1 host packet); replies received
by a host are not logged. `timestamp` is the core's wireless timestamp. It works in any state and
also when debug is off.

`mds_packets_take` copies headers and packs the packet bytes back to back into `data`, and returns
the number of packets copied. A packet that does not fit in what is left of `max_data` is left for
the next call, unless it is the **first** of the call: then its first `max_data` bytes are copied
(`length` = `max_data` < `total`) and the rest of it is dropped (level 6). So the call never blocks:
up to level 5 a packet longer than `max_data` was never returned and every call returned 0. Staged
like the others: loop until 0. `max_packets` and `max_data` must not be negative. `total` was the
struct's trailing padding, so the layout is unchanged; older libraries leave it 0.

`mds_debug_packet` (level 6) is a test hook: it logs a packet as the platform layer does for the
console's own traffic (`sent` 1 for a sent packet, `type` as above, timestamp 0), if capture is on.

### Persistence of debugger data

| Data | `mds_reset` / `mds_load_rom` | `mds_loadstate` | Cleared by |
| --- | --- | --- | --- |
| breakpoints, watchpoints, hits | kept | kept | `mds_point_remove` |
| trace ring | emptied (config kept) | kept (a log of what ran) | `mds_trace` |
| call stacks | cleared | **cleared** (tracking stays on; re-seeded from stack memory at the next call) | `mds_calls(id, 0)` |
| a watchpoint hit not yet taken | cleared | **cleared** | parking on it |
| call counts | kept | kept (a log) | `mds_callcounts_take` |
| coverage | kept | kept (a log) | `mds_coverage_take` |
| last-writer table | zeroed | **zeroed** (tracking stays on) | `mds_lastwriter(id, 0 or 1)` |
| armed step / break requests | cleared | kept (they are requests about the future) | resuming into them; every park and the end of a run drop the break requests (level 6), steps stay |
| packets | kept | kept | `mds_packets` |
| staged rest of a cut-short take | kept | kept | the next take |
| probes, memory log ranges, actions (level 7) | kept | kept | `mds_probe_remove`, `mds_memlog_remove`, `mds_action_remove` |
| probe, memory log and action records | kept | kept (a log) | the takes; `mds_probe_capacity` / `mds_memlog_capacity` empty their rings |
| calls awaiting their return (probes) | given up | **given up** (`MDS_PROBE_RETURN_LOST`) | their return |
| action hit counts, actions applied (`mds_action_stats`) | kept | kept | never (the counts of a removed action go with it) |
| tagged call counts and coverage | kept | kept (a log) | `mds_callcounts_take_ov`, `mds_coverage_take_ov` |
| the overlay table of the attribution | reread (the ROM may have changed) | kept (the cache checks memory) | - |

The rule for a load (`Debugger::OnStateLoaded`, also used by the Qt frontend): what describes the
execution or memory before the load is dropped (call stacks, the parked thread stacks and
interrupted contexts behind them, a pending watchpoint hit, last-writer records); configuration
(points, trace range, tracking switches, armed steps and break requests) and accumulated logs
(trace, call counts, coverage, packets) are kept. API level 1 kept everything over a load. From level 4
a load also arms the stack seeding (the frames below the first call made after the load are read back
from stack memory) and forgets the calls that `memory.execute` was waiting to return from.

---

## 5b. Probes, the memory log, breakpoint actions and overlay tags

API level 7 (`feature/probes`), declared in `melonds_probes.h` (included by `melonds_api.h`), implemented in
`src/debug/Probes.*` (core) and `src/frontend/headless/ApiProbes.cpp`. Four tools that log or act inside the run
**without parking** the console, so a whole scenario can run with them on:

| feature (`mds_has`) | calls | what it does |
| --- | --- | --- |
| `probes` | `mds_probe_add`, `_remove`, `_list`, `_capacity`, `_take`, `_stats` | every call of a function: r0-r3, memory behind registers, LR, SP, call depth and overlay at entry; r0, r1 (and memory) at its return |
| `memlog` | `mds_memlog_add`, `_remove`, `_list`, `_capacity`, `_take`, `_stats` | every CPU (and DMA) read and/or write in address ranges: frame, cycle, CPU, PC, address, size, value |
| `bp_actions` | `mds_action_add`, `_remove`, `_list`, `_take`, `_stats` | at an address: return early with a value, set a register, write memory or skip instructions; each application logged |
| `overlay_tags` | `mds_overlay_tags`, `mds_overlay_at`, `mds_callcounts_take_ov`, `mds_coverage_take_ov` | which cart overlay the code at an address belongs to now; call counts and coverage per (address, overlay) |

Every call needs a console created with the debugger and **stopped** (idle or parked), like the other debugger
calls (`console N is running`, `... created without the debugger (debug = 0)`), and runs on the emulator thread.
Ids of probes, ranges and actions come from one counter per console (1, 2, ...; never reused), separate from the
point ids. The takes are staged like `mds_trace_take`: each call returns what fits and keeps the rest; loop until
0. Records carry `frame` (the `mds_frames` value of the frame they happened in) and `cycle` (the CPU's timestamp,
ARM9 in system cycles, as `mds_trace_entry.cycle`), so records of all three logs and the trace can be ordered.

**Cost and determinism.** Probes and actions arm the instruction hook of their CPUs (as a breakpoint does: one hook
call per instruction and a 1 MiB page table lookup); the memory log arms only the data hook (one call per load or
store, and per DMA unit); none of them adds emulated cycles. Measured on a DQ9 battle round (720 frames,
`woodus-battle-round2-muriel-target`, interpreter with debug on): no hooks 176-182 fps; a probe on `NextRandomMax`
(47 calls) x1.16-1.26; a probe on the hottest function (219,052 calls, every record taken) x1.44-1.49; the memory log
over the hero's 0x964-byte party record x1.06; probe + memory log + call tracking + overlay tags x1.76-1.85; call
tracking alone (as dq9-re's diffcov) x1.57-1.64. Every case ended with the same RAM as the run without hooks:
probes, the memory log and overlay tags only read memory (without side effects, as `mds_read`) and do not change
the run. **Actions change the run** by design; each is logged so a run that used one can be marked.

### Function probes

```c
typedef struct { int32_t reg; int32_t offset; uint32_t len; uint32_t when; } mds_probe_read;
typedef struct {
    uint32_t cpumask, addr, lr_start, lr_end, flags;   /* flags: MDS_PROBE_NO_RETURN */
    mds_probe_read reads[4];
} mds_probe_spec;

int mds_probe_add(int id, const mds_probe_spec* spec);
int mds_probe_remove(int id, int probe);
int mds_probe_list(int id, int* probes, mds_probe_spec* specs, uint64_t* hits, int max);
int mds_probe_capacity(int id, int capacity);
int mds_probe_take(int id, mds_probe_record* out, uint8_t* data, int max, int max_data);
int mds_probe_stats(int id, uint64_t* out4);
```

A probe watches the instruction at `addr` (bit 0 ignored; `cpumask` 1 ARM9, 2 ARM7, 3 both). Each time a CPU is
about to execute it, a record is made with r0-r3, LR, SP, the call depth (`mds_calls` on: the call tracker's
frames, the call's own included; else -1), the overlay at `addr` (ARM9; see [Overlay tags](#overlay-tags)) and up to
four **reads**: `len` bytes (at most 256) from register `reg` + `offset` (`reg` 15 is the probed address), taken
with `when` = `MDS_PROBE_READ_ENTRY` at entry and/or `MDS_PROBE_READ_RETURN` at the return (from the register's
value at entry: an out-parameter). Reads go through the debugger (main RAM directly, TCM as the CPU sees it, I/O
reads 0). `lr_start`/`lr_end` keep only calls whose return address (LR, bit 0 cleared) is in the range (0, 0: every
call); `hits` counts the calls kept.

**Returns.** Unless `MDS_PROBE_NO_RETURN`, the call is awaited: its return is the first time that CPU reaches the
return address (LR at entry, bit 0 cleared) with the SP and CPU mode it had at entry. Then `returned` = 1, `ret` =
r0, r1, `ret_frame`/`ret_cycle` are set and the return reads are appended. This matches tail calls (the callee
returns to the caller's caller with its SP), recursion (the innermost awaited call returns first) and NitroSDK
thread switches and interrupts (other stacks, other modes). A call whose return never comes that way (longjmp, a
function entered without a real return address in LR, a thread that never resumes) stays awaited: at most 256 per
CPU, the oldest given up beyond that, and all of them at a state load (`MDS_PROBE_RETURN_LOST` in their records).
*(inferred)* A function entered by a jump with LR holding something else than its return address is matched at
whatever LR points to, if ever.

```c
typedef struct {
    uint64_t seq, frame, cycle, ret_frame, ret_cycle;
    int32_t probe, cpu, kind, overlay, depth;
    uint32_t addr, lr, sp, args[4], returned, ret[2], flags;
    uint16_t entry_len[4], ret_len[4];
    uint32_t data_len, pad;
} mds_probe_record;   /* 128 bytes */
```

Records go into a ring of `capacity` records (`mds_probe_capacity`, default 65536, which also empties it); when it
is full the oldest is dropped and counted. `seq` numbers the calls from 0 per console, so a gap shows dropped
records. A record is made at entry and completed in place at the return; **a call taken before it returned**
(`mds_probe_take` while it runs, for example a take at every frame boundary) comes back with `returned` = 0, and its
return arrives later as a record of `kind` 1 (a late return) with the same `seq`, `ret`, `ret_frame` and the return
reads (`frame`, `cycle`, `args` and `depth` are 0/-1 there). `flags`: `MDS_PROBE_FORCED` when a breakpoint action
fired at the same instruction (the return is the forced one), `MDS_PROBE_RETURN_LOST` when the return was given up.

`mds_probe_take` copies up to `max` records, oldest first, and packs each record's bytes into `data`: the entry
reads in order (`entry_len[i]` bytes for read i, 0 for none), then the return reads (`ret_len[i]`); `data_len` is
the total. A record whose bytes do not fit in what is left of `max_data` is left for the next call, unless it is the
first of the call: then its first `max_data` bytes are copied (`data_len` smaller than the lengths' sum).
`mds_probe_stats` fills `out4` with: records made, records dropped from the full ring, calls awaiting their return,
records in the ring. Errors: `probe read N: register must be 0-15 or -1`, `... at most 256 bytes`, `... when must
be MDS_PROBE_READ_ENTRY and/or _RETURN`, `cpumask selects no CPU`, `no probe N`.

Probes are kept over resets, ROM loads and state loads; the awaited returns are not (a reset or load starts the CPUs
elsewhere). Tests: `test_probes.py` (`Probes`, `LateReturn`, `Determinism`).

### Memory access log

```c
typedef struct { uint32_t cpumask, start, size, flags, pc_start, pc_end; } mds_memlog_spec;
/* flags: MDS_MEMLOG_READ 1 | MDS_MEMLOG_WRITE 2, | MDS_MEMLOG_DMA 4 */
typedef struct {
    uint64_t seq, frame, cycle;
    uint32_t pc, addr, value;
    int32_t range;
    uint8_t cpu, size, flags, dma;
    uint32_t pad;
} mds_memlog_record;   /* 48 bytes */

int mds_memlog_add(int id, const mds_memlog_spec* spec);
int mds_memlog_remove(int id, int range);
int mds_memlog_list(int id, int* ranges, mds_memlog_spec* specs, uint64_t* hits, int max);
int mds_memlog_capacity(int id, int capacity);
int mds_memlog_take(int id, mds_memlog_record* out, int max);
int mds_memlog_stats(int id, uint64_t* out4);
```

A range logs every read and/or write of the CPUs in `cpumask` that overlaps `[start, start + size)` (`size` > 0, at
least one of read and write), as watchpoints would see them but **without parking**: CPU loads and stores
(including TCM) and, with `MDS_MEMLOG_DMA`, DMA units of that CPU's DMA controller (both the source read and the
destination write, unlike watchpoints, which see DMA writes only). `pc_start`/`pc_end` keep only accesses made by
instructions in that range (0, 0: any); DMA units pass it. One record per access, for the first range (in creation
order) that takes it:

- `pc`: the instruction that made the access; for a DMA unit the DMA tag `0xFFFFFFF0 | channel << 1 | arm7` (as
  watchpoint breaks and last-writer records), with `flags & MDS_MEMLOG_DMA` and the channel in `dma` (0xFF for an
  instruction).
- `value`: the value written; for a read, memory at `addr` read through the debugger as the access starts (what the
  instruction reads; I/O reads 0, as `mds_read`). `size` 1, 2 or 4; `flags` `MDS_MEMLOG_READ` or `_WRITE`.
- `cpu` 9 or 7, `range` the range id, `seq` the access number per console from 0.

The ring holds `capacity` records (`mds_memlog_capacity`, default 1 << 20, 48 MiB at most; it empties the ring);
when full the oldest are dropped and counted (`mds_memlog_stats`: made, dropped, 0, in the ring). The log does not
need the instruction hook: the pc is taken from R15 inside the data hook (8 ahead of an ARM instruction, 4 of a
Thumb one), which was checked against the instruction hook's pc over every data access of a DQ9 battle round. Ranges
are kept over resets and loads. Errors: `memlog: size must be > 0`, `memlog: flags must have MDS_MEMLOG_READ and/or
MDS_MEMLOG_WRITE`, `no memlog range N`. Tests: `test_probes.py` (`MemLog`).

### Breakpoint actions

```c
typedef struct {
    uint32_t cpumask, addr;
    int32_t kind, reg;            /* MDS_ACTION_RETURN 1, _SET_REG 2, _WRITE 3, _SKIP 4 */
    uint32_t value, value2, target, size, lr, first, times, flags;   /* flags: MDS_ACTION_SET_R1 */
} mds_action_spec;
typedef struct {
    uint64_t seq, frame, cycle, hit;
    int32_t action, cpu, kind;
    uint32_t pc, lr, regs[4], value, target, pad;
} mds_action_record;   /* 80 bytes */

int mds_action_add(int id, const mds_action_spec* spec);
int mds_action_remove(int id, int action);
int mds_action_list(int id, int* actions, mds_action_spec* specs, uint64_t* matched, uint64_t* fired, int max);
int mds_action_take(int id, mds_action_record* out, int max);
int mds_action_stats(int id, uint64_t* out4);
```

An action does at `addr`, before the instruction there executes and without parking, what a client would do at a
breakpoint with `mds_set_reg` and `mds_write`:

| kind | effect |
| --- | --- |
| `MDS_ACTION_RETURN` | r0 = `value` (and r1 = `value2` with `MDS_ACTION_SET_R1`), then PC = LR (bit 0 picks Thumb, as `bx lr`): the function returns at once without running its body (a knockout, or a forced result) |
| `MDS_ACTION_SET_REG` | register `reg` (0-15) = `value`; 15 jumps, keeping the ARM/Thumb state as `mds_set_reg` does |
| `MDS_ACTION_WRITE` | writes `size` (1, 2, 4) bytes of `value` at `target` through the CPU's bus (not I/O); code written there takes effect (`pipeline_refetch`) |
| `MDS_ACTION_SKIP` | PC += `size` instructions: the instruction at `addr` and the next `size - 1` do not run |

Matching: `lr` (bit 0 cleared; 0 = any) keeps only hits with that return address in LR; of the matching hits it
fires from the `first`-th (1-based, 0 = 1) at most `times` times (0 = every one from `first`). So `first` = k,
`times` = 1 is the dq9-re harness's `--force-roll ADDR=VALUE@LR#k`. `matched` and `fired` count per action.

An action that moves PC (return, skip, set r15) means the instruction at `addr` does not run: the hooks run again at
the new PC (level 4's `step_redirect`), where a breakpoint fires and an armed step counts from there. A breakpoint at
`addr` itself does not fire then, and the other actions at `addr` (in creation order) are not applied. A probe at
`addr` still records the call (entry first, then the actions), with `MDS_PROBE_FORCED`, and its return is the forced
one. Actions that do not move PC (a register other than 15, a write) let the instruction and the breakpoints there
run as usual.

Every application is logged: `mds_action_take` returns the last 65536 (oldest first; older ones are dropped and
counted) with the hit number, pc, LR, r0-r3 before the action and the value. `mds_action_stats` `out4[0]` counts
every action applied since the console was created and is never reset (not by a take, a reset or a load), so a
client can check before and after a run whether anything was forced. Errors: `action: register must be 0-15`,
`action: write size must be 1, 2 or 4`, `action: I/O cannot be written (side effects)`, `action: skip at least 1
instruction`, `action: unknown kind N`, `no action N`. Tests: `test_probes.py` (`Actions`).

### Overlay tags

```c
int mds_overlay_tags(int id, int enabled);
int mds_overlay_at(int id, uint32_t addr);   /* INT32_MIN on error */
int mds_callcounts_take_ov(int id, int cpu, uint32_t* entries, int32_t* overlays, uint64_t* counts, int max);
int mds_coverage_take_ov(int id, int cpu, uint32_t* addrs, int32_t* overlays, int max);
/* MDS_OVERLAY_STATIC -1, MDS_OVERLAY_AMBIGUOUS -2, MDS_OVERLAY_UNKNOWN -3 */
```

Several overlays can occupy one address range (DQ9 has six regions for 35 overlays), so an address alone does not
say which function ran there. The core attributes ARM9 code to the cart overlay whose **image matches the memory at
the address at that moment**, with no game-specific table: the cart's ARM9 overlay table and images are read once
per ROM (as `mds_overlays` reads them, decompressed), overlays whose code ranges overlap form regions, and for an
address inside a region the candidates are the overlays whose code covers it. The candidate with the most equal
words in a 16-word window from the address wins (64 words if that ties); a remaining tie goes to the overlay whose
first 8 words match memory best (the region-start rule); fewer than half the words equal means no overlay's code is
there. A small cache per address, validated against the memory at the address and at the region's start, makes a
repeat lookup a handful of reads. `mds_overlay_at` returns the id, `MDS_OVERLAY_STATIC` (no overlay covers the
address: the main module, TCM, ...; also every ARM7 address), `MDS_OVERLAY_AMBIGUOUS` or `MDS_OVERLAY_UNKNOWN` (in
an overlay range but matching none: an all-bss overlay, cleared or overwritten memory).

A patched instruction (an Action Replay code) or a stale tail left by a smaller overlay does not change the answer
(the window still matches the overlay whose code is there). Checked on DQ9 (a battle round sampled every 4 frames,
1,101 samples): every resident overlay with code agreed with the game's own slot table (`0x01FFD364`) and with
dq9-re's detection; the only difference was ov032, which is all bss (`UNKNOWN`). Every one of the 610
overlay-tagged call entries of that round starts a symbol of its overlay.

`mds_overlay_tags(id, 1)` also keeps **call counts and coverage per (address, overlay)**, beside the untagged ones:
`mds_callcounts_take`, `mds_coverage_take` and their outputs are unchanged, and the tagged ones are taken with
`mds_callcounts_take_ov` (each (entry, overlay) pair with its count, in no particular order) and
`mds_coverage_take_ov` (each (address, overlay) pair, sorted by address then overlay). They need call tracking
(`mds_calls`) and coverage (`mds_coverage`) on, as the untagged ones do; they accumulate until taken and survive
resets and loads; turning tags off keeps them. Probe records carry the overlay at the probed address in `overlay`.
Cost: an attribution per call (a cache hit is 8 word reads) and, for tagged coverage, one each time execution moves
to another 4 KiB page or overlay range. Tests: `test_probes.py` (`OverlayTags`; without a cart everything is
static), `debug_common_tests` (`OverlayMatch*`, the matching on synthetic images).

### Python

`melonds_mcp.native.Console` (and dq9-re's `melonds/probes.py`, which adds the same methods to its copy):

| C | Python |
| --- | --- |
| `mds_probe_add` | `add_probe(addr, cpu=9, reads=(), lr_range=None, returns=True)`; reads: `(reg, len[, offset[, when]])`, reg `"r1"` or 1, when `"entry"`/`"return"`/`"both"` -> id |
| `mds_probe_remove` / `_list` / `_capacity` / `_stats` | `remove_probe(id)` / `probes()` / `probe_capacity(n)` / `probe_stats()` |
| `mds_probe_take` | `take_probes(pending=None)` -> list of dicts (`"reads"`, `"ret_reads"`: bytes per read); pass one dict as `pending` to every take and late returns are merged into the records taken earlier |
| `mds_memlog_add` | `memlog_add(start, size, read=True, write=True, dma=True, cpu=9, pc_range=None)` -> id |
| `mds_memlog_*` | `memlog_remove(id)`, `memlog_ranges()`, `memlog_capacity(n)`, `take_memlog()`, `memlog_stats()` |
| `mds_action_add` | `add_action(addr, kind, value=0, cpu=9, reg=-1, value2=None, target=0, size=0, lr=None, first=1, times=0)`; kind `"return"`, `"set_reg"`, `"write"`, `"skip"` -> id |
| `mds_action_*` | `remove_action(id)`, `actions()`, `take_actions()`, `action_stats()` (`"applied"`) |
| `mds_overlay_tags` / `_at` | `overlay_tags(enabled=True)` / `overlay_at(addr)` |
| `mds_callcounts_take_ov` / `mds_coverage_take_ov` | `take_call_counts_ov(cpu=9)` -> `{(entry, overlay): n}` / `take_coverage_ov(cpu=9)` -> `[(addr, overlay)]` |

```python
c.load_state("work/battle.mln")
p = c.add_probe(0x020742FC)                     # NextRandomMax(Random*, n): n in r1, the draw in r0 at return
c.memlog_add(0x020F5478, 0x964)                  # the hero's party record
pending, calls = {}, []
for keys, frames in steps:
    c.set_keys(keys)
    c.run(frames)
    calls += c.take_probes(pending)              # take every step: the ring never fills
for r in calls:
    print(r["frame"], hex(r["lr"]), r["args"][1], r["ret"] and r["ret"][0])
for a in c.take_memlog():
    print(a["frame"], hex(a["pc"]), "w" if a["write"] else "r", hex(a["addr"] - 0x020F5478), hex(a["value"]))
```

Not in the socket protocol, the CLI, the MCP tools or Lua yet (see [section 11](#11-known-issues-and-surprises)).

---

## 6. Savestates

```c
int mds_savestate(int id, const char* path);
int mds_loadstate(int id, const char* path);
```

Both need an **idle** console: a running or parked one fails with "savestates need a frame
boundary; finish the frame first" (test `test_parked_console_gets_console_running`); finish the
frame with `mds_start(id, 0)` and `mds_wait` first. They run on the emulator thread.

- `mds_savestate` writes melonDS's own format (magic `MELN`), version **14.2**. Errors: "savestate
  failed", "cannot write PATH".
- `mds_loadstate` reads the file, empties the input queue and the mic FIFO, takes a backup of the
  current state, and loads. A rejected file restores the backup ("savestate rejected: PATH"); a
  missing one fails with "cannot read PATH". A load through the socket server also sends an
  `input_queue` event with depth 0.
- **Loading writes the state's save memory into the console's save file.** This is deliberate and
  kept for now: the cart's save memory is part of the emulated console, and after a load the file
  follows what the console now holds (as it does after every save write by the game). It matters
  because a save file booted in place is overwritten by the first load, with the save memory of
  whatever run made the state. **Clients must boot a copy** of any save they want to keep (the
  dq9-re harness copies each save to a temp folder and boots the copy). Future work may make this
  optional.

**Versions.** 14.0 is upstream melonDS. **14.1** (fork commit `d8a5e8d9`) adds the hardware divider
and square-root operands and results (`DIV_NUMER`, `DIV_DENOM`, `DIV_RESULT`, `DIVREM_RESULT`,
`SQRT_PARAM`, `SQRT_RESULT`): the SDK copies the operands into thread contexts, so a 14.0 save, run,
load round trip could diverge. **14.2** (`fix/api-quirks`) adds:

- `KeyInput` (buttons, the pen-down bit and the lid bit; upstream left it out as a TODO),
- the touch controller's pen position,
- the core's mic input buffer (samples already taken from the platform), and
- a headless section `HDLS` after the core's: the held input the empty-queue rule repeats (state,
  source, empty frames since), the pen state the touch timeline ended the frame in, the emulated
  frame count and the samples queued in the mic FIFO.

Loading accepts major 14 with minor 0, 1 or 2. An older state loads as before: what it lacks keeps
its current value (the divider values for 14.0; for 14.0 and 14.1 `KeyInput`, the pen, the held
input, the emulated frame and the mic buffers stay as they were before the load, which empties the
mic buffers). A state from a newer minor is rejected ("state from the future"); builds before this
branch reject 14.2 states. Another frontend's 14.2 state (no `HDLS`) loads; the held input is then
taken from the loaded `KeyInput` and pen position.

What a state contains: the whole emulated console (both CPUs and their banked registers, CP15
and both TCMs, all memory, video, sound, DMA, timers, the RTC, Wi-Fi, the cart and its save memory),
plus (14.2) the input that affects emulation as described above.

What it does not contain (and what happens on load):

- The process frame counter (`mds_frames`), the input log, the input mode and stall timeout, and
  Lua state are frontend state and are not touched by a load.
- Action Replay codes: the cheat list is not saved, and a successful load **clears** it (level 3;
  [section 6a](#6a-action-replay-cheat-codes)). What the codes already wrote to memory is RAM like
  any other and is in the state.
- Debugger data: a load clears what described the execution before it (call stacks, a pending
  watchpoint hit, last-writer records) and keeps points, logs and armed requests (see the table in
  [section 5](#persistence-of-debugger-data)).
- The framebuffers: the screen shows the pre-load frame until frames have run.
- Caches: this melonDS version emulates no ARM9 data cache, and although instruction-cache arrays
  exist they are not used for fetches (`ICacheLookup` is commented out in `CP15.cpp`), so nothing
  cache-related is missing from a state.
- JIT is off in headless, so states are taken from the interpreter's real pipeline.

Derived state (level 9, feature `savestate_derived`). Some CPU state is not in the format and is
derived on load:

- **The CPUs' interrupt lines** (`ARM::IRQ`, which the core keeps equal to `IME & (IE & IF)` and
  updates on every write to those registers) are recomputed with `UpdateIRQ` for both CPUs. Before
  level 9 the line kept its pre-load value: after a load of a state with an interrupt pending
  (raised while the CPU had IRQs masked), a console whose line was clear did not take it when the
  game unmasked IRQs, until the next write to IME, IE or IF or the next raised interrupt.
- **The ARM9's code-fetch timing** (`RegionCodeCycles`, set by every branch from the protection
  unit and cache settings) is taken from the loaded CP15 state. Before level 9 it was read before
  CP15 was loaded, from the timing tables of whatever the console ran before: a console fresh from a
  reset fetched the game's cached code at uncached speed (18 cycles instead of 1-3) until the
  game's first branch, while a console that had been running the game fetched it at cached speed.
  The first instructions after a load therefore took a different number of cycles on the two, and
  that was enough to shift when interrupts, DMA and GX commands landed: in DQ9 the RAM differed for
  a frame or two after the load (then converged) and the GX FIFO ring stayed offset for good.
- The JIT idle-loop flag is cleared (only the JIT uses it, and a load resets the JIT).

GX FIFO entries (feature `gx_fifo_zeroed`) are written with zeroed padding: the state holds the
FIFO arrays raw, and bytes 5-7 of each entry used to be uninitialised stack, so two runs that
emulated the same thing could save states that differed in those bytes.

With these, after a load a reused console is byte for byte the same as a fresh console that loaded
the same state, frame after frame (`tools/melonds-mcp/tests/test_savestate_reuse.py`; checked on 15
DQ9 states for 20-30 frames after the load), and a load reproduces the run that saved the state.
**Runs from a loaded state made before level 9 can differ from runs made now** in their first
frames (a fresh console now runs the first instructions at the speed the game ran them); states
themselves are unchanged and load as before. In dq9-re, 9 of 4084 scenario states built from their
parents changed RAM hash (a few timing-dependent loader words; no screen changed), and in 89
re-run diffcov pairs only raw call counts of two GX busy-wait polls moved (by 1-11), never which
functions a pair singles out. Nothing else in the core was found to survive a load: with the
fixes, the saved state of a reused console matched a fresh one in every section.

Determinism: with 14.1 and later, a save, run, load, run sequence reproduces a run that never loaded
(dq9-re replays all scenarios byte for byte; 14.2 states reproduce the RAM and screen hashes
recorded with 14.1). With 14.2 the input held at the save, the pen and the mic samples also carry
over, so a run that relies on the repeat rule after a load, or feeds the microphone, reproduces too.
Before level 9 that held for a fresh console only when the ARM9 code timing taken from the reset
console happened to match (see "Derived state" above); from level 9 it holds for fresh and reused
consoles alike.
Keep to one console per process and set the RTC after loading (the state carries its own clock).

---

## 6a. Action Replay cheat codes

```c
typedef struct {
    int32_t cheat;      /* the code's id in this console's list */
    int32_t enabled;
    int32_t words;      /* code words: two per line */
    char name[116];     /* NUL-terminated, cut short if longer */
} mds_cheat;

int mds_cheat_add(int id, const char* name, const char* code, int enabled);
int mds_cheat_remove(int id, int cheat);
int mds_cheat_enable(int id, int cheat, int enabled);
int mds_cheat_clear(int id);
int mds_cheat_list(int id, mds_cheat* out, int max);
int mds_cheat_get(int id, int cheat, char* name, int name_max, uint32_t* words, int max_words);
```

API level 3, feature `cheats`. Each console has a list of Action Replay (AR DS) codes that melonDS's
own cheat engine (`src/AREngine.cpp`, the engine the Qt frontend's Cheats dialog feeds) runs. The
library adds nothing to the engine: a code here does exactly what it does in the Qt frontend.

**The calls.** All six work in any state and on a console created with `debug = 0` (cheats are not
a debugger feature). They run on the emulator thread through the command queue, so a change never
races the engine: on a running console it lands at the next frame boundary, on an idle or parked
one at once.

- `mds_cheat_add` checks the text, appends the code to the list (enabled when `enabled != 0`) and
  returns its id: 1, 2, ... per console, never reused (not after a remove, a clear or a load).
  `name` may be NULL (stored as ""); it is a label only and need not be unique.
- `mds_cheat_remove` / `mds_cheat_enable` take that id; an unknown one fails with `no cheat N`.
- `mds_cheat_clear` empties the list and returns how many codes it held.
- `mds_cheat_list` copies up to `max` entries in list order and returns the number copied; `name`
  is cut to 115 bytes.
- `mds_cheat_get` copies one code's whole name (at most `name_max - 1` bytes and a NUL; `name` may be
  NULL when `name_max` is 0) and up to `max_words` of its words, and returns its word count (which
  may exceed `max_words`). The words are the parsed code, so the text comes back normalised
  (`%08X %08X` per line).

Negative `max`, `name_max` or `max_words` fail (`... must not be negative`).

**Code text.** One `XXXXXXXX YYYYYYYY` line per pair of words: two 8-digit hex words, any case,
separated by spaces or tabs; CR LF or LF line ends; blank lines are skipped. Nothing else is
accepted (no `0x`, no comments, no codes run together on one line). Before anything is added, the code
is checked as the engine will read it, and a bad one fails with `mds_error` = `cheat: ` plus the
reason, naming the line:

| Error (after `cheat: `) | Cause |
| --- | --- |
| `the code is empty (no "XXXXXXXX YYYYYYYY" lines)` | no code lines, or `code` is NULL (`cheat: no code text`) |
| `line N: expected two 8-digit hex words ("XXXXXXXX YYYYYYYY"), got "..."` | a malformed line |
| `line N: opcode C4 (offset = address of the code) is not supported by melonDS's AR engine` | the engine logs and stops at C4 |
| `line N: unknown opcode XX (melonDS's AR engine would stop there)` | C1-C3, C7-CF, DD-DF |
| `line N: unknown D4 data operation XX (00-08 exist)` | the engine would skip it with a warning |
| `line N: the E opcode copies B bytes (L lines of data) but only M lines follow` | an `E` (copy parameter bytes) line without its data; the engine would read past the code |
| `the code is longer than 4096 lines` / `the name is longer than 4096 bytes` | limits |

The opcodes the engine runs (`00`-`BF` writes and conditions, `C0` loop, `C5` counter, `C6`, `D0`-`DC`,
`E` copy, `F` memory copy) are as in GBAtek's "DS Cheat Action Replay"; the engine's comments in
`AREngine.cpp` say which. Data lines of an `E` code are data and are not checked.

**When codes run.** Not at the frame boundary: inside the frame, when the **ARM7 takes an IRQ while
the VBlank IRQ is pending and enabled** (`ARM::TriggerIRQ`, `IF & IE` has bit 0), which is how an AR
cartridge hooks the ARM7's VBlank handler. The VBlank IRQ is raised at scanline 192 of the 263 a
frame has (melonDS's frames run from line 0 to 262), so in a game codes run once per frame, about
73% of the way through it, before the ARM7's own handler runs. Consequences:

- A game (or test program) that never enables the ARM7 VBlank IRQ, or keeps IRQs masked, never runs
  the codes. FreeBIOS with no cart is such a case: the tests in `tools/melonds-mcp/tests/test_cheats.py`
  run an ARM7 program that enables the IRQ.
- If the ARM7 takes a second IRQ before it acknowledges VBlank, the codes run again in that frame
  (the engine's rule; a game's handler normally acknowledges at once).
- Codes run in list order; within a code the engine resets its offset and data registers each time
  it starts the code. The `C5` counter is per run, not kept between frames.
- A code added, enabled or removed takes effect from the next VBlank IRQ the ARM7 takes. Removing or
  disabling a code does **not** undo what it wrote: a patched instruction stays patched until the
  game (or an overlay load) rewrites it.
- The writes use the ARM7's bus (`NDS::ARM7Write*`), so codes see and write memory as the ARM7 does:
  main RAM, shared WRAM as mapped to the ARM7, ARM7 WRAM and I/O. ARM9 TCM is not reachable.
- Timing is emulated time, so codes are as deterministic as the rest of the run: the same state, the
  same codes added at the same point and the same input give the same result every time.

**Codes and the debugger.** The engine's writes bypass the CPU access hooks: they **do not trigger
watchpoints** and are **not recorded by the last-writer table** *(verified:
`test_cheat_writes_bypass_watchpoints_and_last_writer`)*. Call counts, call stacks, coverage, traces and
breakpoints keep working while codes run, but they describe the code that actually ran: **a code that
patches game code changes which functions run** (a patched `BL` calls something else, a patched
branch skips a function), so a call count or coverage diff of a run with such a code shows the
patched function missing (or a new one present) because of the code, not because of the input. Mark
such runs (dq9-re records the codes in every output and scenario sidecar). The test
`test_call_counts_and_coverage_follow_the_patch` patches a `BL` and checks that counts and coverage
move to the new target, and stay there after the code is removed.

**Codes and savestates.** The cheat list is **not part of a savestate**: `mds_savestate` writes no
codes, and a **successful `mds_loadstate` clears the list** (a rejected load keeps it). A state
then describes a run completely: loading it and playing the same input gives the same result no
matter what codes were added before the load. A client that wants codes after a load **re-adds them
after loading** (the dq9-re harness adds them right after each load, before the inputs). What the
codes wrote before the state was saved is ordinary memory and is in the state: a state saved while a
code patched game code keeps that patch, and runs from it behave patched until the game rewrites the
code, even with no codes in the list.

| Event | Cheat list |
| --- | --- |
| `mds_loadstate` (success) | cleared |
| `mds_loadstate` (rejected) | kept |
| `mds_load_rom` | cleared (codes are written for one game) |
| `mds_reset` | kept (a power cycle with the AR cartridge still in) |
| `mds_savestate` | not saved |

Lua and the socket server have no cheat calls yet; a code added through the C API also runs for
them.

---

## 7. Lua

```c
int mds_lua_run(int id, const char* source, int is_file);
int mds_lua_exec(int id, const char* code);
int mds_lua_stop(int id);
int mds_lua_output(int id, char* out, int max);
```

Each console has one Lua state. All four need a stopped console except `mds_lua_output`
(whose `max` must not be negative).

All three run on the console's **emulator thread** through the command queue, like the other calls
that touch the console, so they are serialised with every other caller (another thread, the socket
server) and the script only ever touches the console from that thread.

- **`mds_lua_run`** discards the current state (stopping any script and removing its hooks),
  creates a fresh one, and loads `source` (a file path when `is_file`, else code) as the main
  coroutine. The body runs **before the call returns**, until its first `emu.frameadvance()` or its
  end; afterwards it is resumed at the end of every frame. A syntax error, and a runtime error in
  that first step, fail the call with the Lua message (the script is then gone). A runtime error in
  a later step is printed to the output (`lua: ...`) and ends the script.
- **`mds_lua_exec`** runs a chunk in the same state (globals shared with the script). A Lua error
  fails the call with the message.
- **`mds_lua_stop`** discards the state, its script and hooks.
- **`mds_lua_output`** removes up to `max` bytes of printed text and returns how many (no NUL
  terminator; loop until 0). Each `print` adds a newline. While a socket client is connected the
  server takes the output (it becomes `lua_output` events).

Order at the end of each frame (emulator thread): `event.onframeend` callbacks, then the script
coroutine is resumed, then the script's input is submitted as one payload.

Functions available (`LuaHost.cpp`, `lua_debug/LuaDebug.cpp`):

| table | functions |
| --- | --- |
| `memory` | `read_u8` `read_s8` `read_u16_le` `read_s16_le` `read_u24_le` `read_s24_le` `read_u32_le` `read_s32_le` `readbyte`, the matching `write_*` and `writebyte`, `read_bytes_as_array(addr, len)`, `write_bytes_as_array(addr, table)`. Optional last argument: `"Main RAM"` (address relative to 0x02000000), a name starting with `ARM7` (ARM7 bus), `ARM9`/`"System Bus"` (ARM9 bus). Same rules as `mds_read`/`mds_write` (I/O reads 0; writes as `mds_write`: halfwords for ARM9 palette/VRAM/OAM, and a write that cannot be made raises `memory write failed: ...` naming the region, level 6). |
| `emu` | `framecount()` (= `mds_frames`), `frameadvance()` (yields; only in the script body), `getsystemid()` ("NDS"/"DSi"), `getconsoleid()` (effective id), `getregister(name, [cpu])`, `getregisters()`, `setregister(name, value, [cpu])` (names `r0`-`r15`, `sp`, `lr`, `pc`, `cpsr`, optional `ARM7 `/`ARM9 ` prefix; `pc` = R15 - 4/2; `cpsr` read-only) |
| `joypad` | `set{A=true, ...}` (key names as in the bit table; nil keys unchanged), `get()` (the pending state if set this step, else the applied one) |
| `input` | `NDSTapDown(x, y)`, `NDSTapUp()` |
| `savestate` | `save(path)`, `load(path)`; they only succeed while the console is idle, i.e. from `mds_lua_exec`/the start of `mds_lua_run`, not from a running script or hook |
| `event` | `onmemoryexecute(fn, addr, [name], [cpu])`, `onmemoryread(fn, addr, [name], [cpu], [size=1])`, `onmemorywrite(...)`, `onframeend(fn, [name])`, `unregisterbyid(id)`, `unregisterbyname(name)` |
| `debugger` | `callstack([cpu])` (frames `{entry, site, ret, sp, mode, exception}`, innermost first), `threadstacks([cpu])` (level 4: `{ {sp, frames}, ... }`, as `mds_threadstacks`), `trackcalls(on)`, `callcounts([cpu])` (drains), `coverage(on)`, `draincoverage([cpu])`, `trace([capacity=65536], [cpu], [start], [end])` (one CPU), `draintrace()`, `trackwriters(on)`, `lastwriter(addr)` (returns `pc, cpu` for an instruction, `nil, cpu, channel` for a DMA write, `nil` for none), `pause([cpu])` (break request on one CPU), `step([count=1], [cpu])` (exact, as `mds_step`), `peek(addr, [size=4], [cpu])`; `resume` raises "not available here" in headless. On a console without the debugger all but `peek` and `resume` raise "console N was created without the debugger (debug = 0)" |
| `memory` (level 4) | `execute(op, ...)`: see below |
| `wifi` (level 4) | `hook(fn, [name])`, `inject(data, [rate=2])`, `decode(data, [fcs=true])`: see below |
| global | `print` (to the output buffer) |

Memory hooks are ordinary debugger points with a callback: they get ids from the same counter,
appear in `mds_point_list` (tagged `MDS_POINT_LUA` from level 6; `mds_point_remove` refuses them, so
remove them with `event.unregisterbyid`/`unregisterbyname`), and need the debugger (on a console without it `event.onmemory*`
raises). The callback runs on the emulator thread at
the access or instruction, with arguments `(addr, value, pc)` (for a DMA write `pc` is the DMA
tag `0xFFFFFFF0 | channel << 1 | arm7`); returning `true` parks the console
like a breakpoint (break kind exec/read/write with the hook's id), anything else continues. A write
hook runs **before** the write, for a CPU store and a DMA unit alike: `value` is what is being
written and memory at `addr` still holds the old value (test `WriteHookSeesTheOldValue` in
`test_pr_review.py`). The park a write asks for comes after the write (see the break table in
[section 2](#breaks-what-happens-and-mds_take_break)). Before this, a DMA write hook ran after the
unit was written and saw the new value. That
makes any hook a conditional breakpoint. `event.onframeend` ids are negative. The Lua debugger
tables share their data with the C API (`debugger.draintrace` drains the same ring as
`mds_trace_take`).

**`memory.execute(op, ...)`** (level 4, feature `lua_execute`) runs instructions as if they sat in
front of the hooked one. Opcodes are numbers or one table of numbers, ARM or Thumb by the CPU's
current state (a Thumb `BL` pair as one 32-bit value); an optional string argument names the CPU
(`"arm9"`/`"arm7"`). It only works on the emulator thread inside an exec hook (`event.onmemoryexecute`)
or at a break of that CPU, and raises "memory.execute runs only inside an exec hook or at a break, on
that CPU, with JIT off" anywhere else (from `mds_lua_exec` on an idle console, inside a read/write
hook); on a console without the debugger it raises the debug = 0 error. PC reads as it would at the
hooked instruction. It returns `true` if one of the instructions
branched: the hooked instruction is then skipped and execution continues at the target, so
`memory.execute(0xE12FFF1E)` (`bx lr`; `0x4770` in Thumb) at a function's entry makes the function
return at once (test `LuaExecute`). A call (`bl`, `blx`, `swi`) returns to the hooked instruction with
r0-r3, r12, LR and the flags restored to what they were before `memory.execute`, without firing that
instruction's hooks again; the callee's return value is not kept. With `false` the hooked instruction
runs next. An armed `mds_step` counts the hooked instruction, not the executed ones; after a branch the
target is the first counted instruction (`step_redirect`).

**`wifi`** (level 4, feature `lua_wifi`): `wifi.hook(fn, [name])` sees every frame the console sends
or receives (local multiplayer commands, replies and acks, beacons, management and data frames) as
`{tx, data, rate, channel, time}` with `data` the 802.11 frame including its FCS; return `false` to drop
it or a string to replace it. It returns an id from the point counter (remove it with
`event.unregisterbyid`/`unregisterbyname`; it does not show in `mds_point_list`). `wifi.inject(data,
[rate])` queues a frame (up to 256) to be received as if from the air; `wifi.decode(data, [fcs])`
returns the 802.11 header, the Nintendo MP fields, and the fixed fields and elements of management
frames. Wifi hooks work without the debugger and run on the emulator thread. Dropping, replacing or
injecting frames changes what the console and its partners see, so runs that use them are not
comparable with runs that do not.

---

## 8. GDB stub and socket server

### GDB stub

```c
int mds_gdb(int id, int port9, int port7, int break_on_start);
int mds_gdb_host(int id, const char* host, int port9, int port7, int break_on_start);
```

Needs a stopped console created with `debug != 0` (the stub's checks run only in the debug
interpreter; without the debugger the call fails). It runs on the emulator thread. With both ports
`<= 0` it disables the stub; otherwise it (re)starts one GDB server per CPU on `port9` (ARM9) and
`port7` (ARM7), and `break_on_start` makes both CPUs wait for GDB at their next instruction. A port
`<= 0` next to a valid one is passed to the stub as is.

The stubs listen on **127.0.0.1** (`mds_gdb`, or `mds_gdb_host` with `host` NULL or ""). To debug
from another machine pass an IPv4 address to `mds_gdb_host`: `"0.0.0.0"` for every interface, or one
interface's address. A non-loopback address logs a warning (log level Warn): anyone who can reach
the port controls the emulator, with no authentication. A malformed address fails with "bad host:
...". (API level 1 listened on every interface.) The Qt frontend takes the address from an optional
`Gdb.BindAddress` entry of its config file, also defaulting to 127.0.0.1. While GDB holds a
CPU stopped, the emulator thread is blocked inside the stub, the console still reports
`MDS_RUNNING`, and command-queue calls wait until GDB continues *(inferred)*. Fails with "built
without the GDB stub" when compiled without `ENABLE_GDBSTUB` (on by default). Watchpoints work
through GDB too (README).

Behaviour of a GDB session (level 5, upstream's stub changes):

- **One client at a time, and each starts clean** (`gdb_session_reset`). When a client detaches,
  kills a non-extended session, or closes the socket (EOF is now detected: before level 5 a closed
  socket was taken for "no data" and the stub kept a dead connection), the stub drops the
  connection and clears its breakpoints and watchpoints, no-ack mode and extended mode. The next
  client must send the initial `+` again. The stub's breakpoints are its own: the C API's
  `mds_bp_add` points are not touched.
- **Extended mode** (`gdb_extended`): after `!` (GDB's `target extended-remote`), `vKill` clears
  the stub's points and keeps the connection; without it `vKill` ends the session. `vRun` restarts
  the console (reply `S05`); a running target answers it by breaking first.
- **Restart keeps direct boot**: `monitor reset` and `vRun` call `NDS::Restart`, which resets the
  console and, if the ROM was direct-booted, direct-boots it again (before, a reset from GDB
  dropped to the firmware). The headless library direct-boots a ROM when the console needs it
  (`NDS::NeedsDirectBoot`, no firmware boot), so there a GDB restart boots the game again; the
  console's input queue, debugger points, cheats and Lua state are not reset by it.
- **The C API is told** (level 6, `gdb_reset_event`): a restart runs on the emulator thread while
  the console runs, so it is not a park, but it queues an event of kind 6 (`MDS_BREAK_RESET`) for
  `mds_take_break`: `cpu` the stub's CPU, `pc` that CPU's next instruction, `value` 1 for a
  whole-console restart (`vRun`, `R`, `r`) and 0 for `monitor reset`, which resets only the stub's
  CPU. The socket server forwards it as a `break_` event (kind 6). `mds_reset_count` goes up by one.
  A whole-console restart also restarts the emulated frame count (which seeds the level-mic noise
  and is saved in states) and drops Lua's pending input, as `mds_reset` does; `mds_frames` keeps
  counting. Up to level 5 nothing was reported (test `GdbResetEvent` in `test_api_fixes2.py`).
- **`monitor overlays`** (`gdb_monitor_overlays`): prints `overlay N at 0xBASE-0xEND` for each
  resident ARM9 overlay (as `mds_overlays`), or `no overlays loaded`. The text comes back as GDB
  console output (`O` packets) before `OK`. `monitor reset` resets the CPU as before; any other
  command fails with `E01`.

### Socket server

```c
int  mds_serve(int port, int flags, int* bound);
int  mds_serve_host(const char* host, int port, int flags, int* bound);
void mds_serve_stop(void);
int  mds_serve_wait(int timeout_ms);
void mds_serve_name(const char* name);
```

The wire format is specified in `src/frontend/headless/Protocol.h` (version 1); this section only
covers the C functions and how serving changes in-process behaviour.

- **`mds_serve`** starts one server thread per process, bound to **127.0.0.1**, serving every
  console in the process. `port = 0` lets the OS pick. Otherwise the first free port in
  `[port, port + range)` is used, `range = flags & 0xFFFF` (0 means 32, 1 means only `port`).
  Returns 0 and writes the bound port to `*bound`, or -1: "bad port", "server already running",
  "no free port in A-B".
- **`mds_serve_host`** is `mds_serve` on another IPv4 address (`host` NULL or "" = 127.0.0.1;
  `"0.0.0.0"` = every interface). A non-loopback address prints a warning to stderr: the protocol
  has no authentication, so anyone who can reach the port controls every console. A malformed
  address fails with "bad host: ...". `melonds_server --host ADDR` uses it (default 127.0.0.1).
- **One client at a time.** A second connection gets a `busy` error and is closed.
- **`mds_serve_stop`** joins the thread and closes the sockets; consoles keep their state.
- **`mds_serve_wait(timeout_ms)`** waits (`< 0` forever) for a client's `shutdown` message:
  returns 1 if one was received since `mds_serve`, else 0. The host then calls `mds_serve_stop` and
  exits (this is what `melonds_server.exe` does).
- **`mds_serve_name`** sets the name returned in the `hello` reply (default "melonds"), before or
  after `mds_serve`.

Effects on the in-process API while the server runs:

- `mds_serve`, every client connect and every disconnect set **every console's** empty-queue mode
  to repeat and stall timeout to 15, and consoles created while serving get the server's options.
- While a client is connected the server thread takes every console's break events and Lua output
  (to send them as events): in-process `mds_take_break` and `mds_lua_output` see nothing. On
  connect, earlier breaks and output are discarded.
- On disconnect the server clears every console's input queue and frame subscriptions and stops the
  consoles that client started.

Symbols (`symbols_load`, level 4 feature `symbols_rom`): the server loads a dsd `config.yaml` (or its
folder) or an `address name [size]` text file into the console's own table, as the Qt debugger does.
Overlay images, which overlay detection compares with RAM, are read from the **running ROM** on the
emulator thread (decompressed), so no extract is needed; without a ROM, a local config's
`rom_config` extract is used as before. The config's `delinks` files are read too (they map
addresses to source files for the Qt source view; the server does not use them yet). The headless
library reads local files only: a URL fails with "cannot download URL" (only the Qt frontend
downloads). The protocol has no thread-stacks message yet (use `mds_threadstacks` in-process).

Line info (level 5): a symbol table also takes the DWARF `.debug_line` of an ELF linked with
`-sym on`, matched to modules through the delinked source files. A local decomp's
`config/<region>/arm9/config.yaml` loads `build/<region>/arm9.o` by itself when that file exists
(a missing or unreadable ELF does not fail the load); the Qt source dock uses it to mark the exact
line, the PC line and line breakpoints. No protocol message returns line info yet, so for the
server and the C API this only costs the read of that ELF at `symbols_load`.

### Instance base, MAC and save numbering

```c
int mds_set_instance_base(int base);
int mds_instance_base(void);
int mds_mac(int id, uint8_t* out);   /* 6 bytes */
```

The instance base is a process-wide offset added to each console's id to give its *effective id*.
It can only be set while no console exists ("the instance base can only change while no console
exists") and must be `>= 0`. The effective id is used for:

- the firmware MAC address: for effective id `e > 0`, bytes 3, 4, 5 of the firmware's MAC get
  `+e`, `+e * 0x44`, `+e * 0x10` and byte 0 gets its low two bits cleared (`CustomizeFirmware`);
  effective id 0 keeps the firmware's MAC;
- the default save path (`.sav` for 0, `.sav.e+1` otherwise);
- Lua's `emu.getconsoleid()`.

It does not change the ids the API uses or the wireless slot. Its purpose: several processes
(for example one `melonds_server` per player, `--instance-base N`) get distinct MACs and save files.
`mds_mac` copies the console's current firmware MAC. Tests: `test_consoleid.py`.

---

## 9. Python equivalents

Two copies of the ctypes wrapper exist: `tools/melonds-mcp/melonds_mcp/native.py` (`from
melonds_mcp import native`) and `DQ9/dq9-re/melonds/native.py`. With the `fix/api-quirks` update the
dq9-re copy is the same wrapper with its own library lookup (it needs `MELONDS_HEADLESS_LIB`); before
it, the dq9-re copy lacked the input queue and server functions. Errors raise `native.MelonError`
with the `mds_error` text. Methods are on `native.Console` unless marked as module functions.

Functions are bound on first use: the wrapper loads a library that lacks some of them (an older
build), and only calling a missing one raises `MelonError` ("... is not exported by this
melonds_headless library"). `native.has(name)` and `native.api_version()` tell what is there.

| C function | `native.py` |
| --- | --- |
| `mds_version` | `native.api_version()` |
| `mds_has` | `native.has(name)` (on a level-1 library: whether an `mds_*` symbol exists; features are unknown there) |
| `mds_error` | used by every call (message of `MelonError`) |
| `mds_set_local_dir` | not wrapped (`native.lib().mds_set_local_dir(b"dir")`; argtypes are set) |
| `mds_set_log_level` | not wrapped (`native.lib().mds_set_log_level(n)`) |
| `mds_mp_timeout` | `native.mp_timeout(ms=-1)` |
| `mds_set_pacing` | `native.set_pacing(fps)` |
| `mds_create` | `Console(bios9=None, bios7=None, firmware=None, nickname=None, debug=True)` |
| `mds_destroy` | `close()` (also at interpreter exit; ignores the result) |
| `mds_load_rom` | `load_rom(path, save=None)` |
| `mds_reset` | `reset()` |
| `mds_set_rtc` / `mds_get_rtc` | `set_rtc(datetime)` / `rtc()` -> datetime |
| `mds_run` | `run(frames, timeout_ms=60000)` -> `(timed_out, [state])`; `native.run_group(consoles, frames, timeout_ms)` |
| `mds_wait` | `wait(timeout_ms=60000)`; `native.wait_group(consoles, timeout_ms)` |
| `mds_gdb` / `mds_gdb_host` | `gdb(port9=3333, port7=3334, break_on_start=False, host=None)` (`host` given: `mds_gdb_host`) |
| `mds_start` | `start(frames=0xFFFFFFFF)` |
| `mds_stop_after` | `stop_after(frames=0)` |
| `mds_interrupt` | `interrupt()` |
| `mds_state` | `state` (property) |
| `mds_take_break` | `take_break()` -> `Break(kind, cpu, pc, addr, value, size, id, dma)` or None; `kind` is a string from `native.BREAK_KINDS` (level 6 adds `"reset"`); a DMA break has `pc >= native.DMA_TAG` |
| `mds_frames` | `frames` (property; raises `MelonError` for `MDS_FRAMES_ERROR` from a level-6 library) |
| `mds_reset_count` | `reset_count()` (level 6) |
| `mds_screen` | `screen()` -> `c_uint32` array of 256*384 (`bytes()` gives BGRA) |
| `mds_set_render` | `set_render(skip_2d=False, skip_3d=False)` / `set_render_flags(flags)` (`native.RENDER_SKIP_2D`, `RENDER_SKIP_3D`, `RENDER_SKIP_ALL`; frames to draw before a screenshot: `RENDER_FRAMES_2D` = 1, `RENDER_FRAMES_3D` = 2) (level 8) |
| `mds_set_threaded_3d` | `set_threaded_3d(on=True)` (level 8) |
| `mds_render_stats` | `render_stats()` -> dict named by `native.RENDER_STAT_NAMES` (level 8) |
| `mds_set_keys` | `set_keys(names or mask)` |
| `mds_touch` | `touch(x=None, y=None)` (None = pen up) |
| `mds_set_lid` | `set_lid(closed)` |
| `mds_input_submit` | `submit_input(buttons=0, lid=False, touch=None, mic=None, mic_level=None, seq=0, client=False)` -> depth; `touch` is `[(offset, x, y, down), ...]` or `(x, y)` |
| `mds_input_submit_multi` | `native.submit_multi([(console, kwargs), ...], client=False)` -> depths |
| `mds_debug_recv_pending` | `native.debug_recv_pending(console, pending)` |
| `mds_input_mode` | `input_mode(mode=None, stall_frames=None)`; mode `"repeat"`, `"neutral"`, `"wait"` |
| `mds_input_depth` / `_waiting` / `_clear` | `input_depth()` / `input_waiting()` / `input_clear()` |
| `mds_input_log` | `input_log(from_frame=0, max=4096)` -> list of dicts |
| `mds_read` / `mds_write` | `read(addr, length, cpu=9)` -> bytes / `write(addr, data, cpu=9)` |
| `mds_get_regs` | `regs(cpu=9)` -> dict `r0`-`r12`, `sp`, `lr`, `pc`, `cpsr`, `spsr`, `r15_raw` |
| `mds_set_reg` | `set_reg(reg, value, cpu=9)`; reg is a name (`r0`-`r15`, `sp`, `lr`, `pc`) or index |
| `mds_bp_add` | `add_breakpoint(addr, cpu=9)` (`cpu="both"` or 3 for both) |
| `mds_wp_add` | `add_watchpoint(addr, size, read=False, write=True, cpu=9)` |
| `mds_point_remove` / `_enable` | `remove_point(id)` / `enable_point(id, enabled)` |
| `mds_point_list` / `mds_point_count` | `points()` -> list of dicts, all of them (sized by `point_count()` from level 6; at most 4096 with an older library), each with `"lua"` (a Lua hook) / `point_count()` |
| `mds_step` | `step(count=1, cpu=9)` |
| `mds_trace` | `trace(capacity, cpu="both", start=0, end=0xFFFFFFFF)` |
| `mds_trace_take` | `take_trace()` -> list of `(cycle, cpu, pc, instr, cpsr)` (all of it) |
| `mds_calls` | `track_calls(enabled=True)` |
| `mds_callstack` | `callstack(cpu=9)` -> list of dicts, innermost first |
| `mds_threadstacks` | `thread_stacks(cpu=9)` -> list of `{"sp", "frames"}` (up to 64 stacks, 8192 frames); the MCP tool `callstack(threads=true)` appends them |
| `mds_overlays` | `overlays()` -> list of `{"id", "base", "size"}` (level 5) |
| `mds_callcounts_take` | `take_call_counts(cpu=9)` -> `{entry: count}` (all of it) |
| `mds_coverage` / `_take` | `coverage(enabled=True)` / `take_coverage(cpu=9)` -> sorted list |
| `mds_lastwriter` / `_query` | `track_writers(enabled=True)` / `last_writer(addr)` -> None (no record), `(pc, cpu)` or `("dma", channel, cpu)`; raises `MelonError` for errors (tracking off, running, no debugger). With a level-1 library it falls back to `mds_lastwriter_get`, where errors also give None |
| `mds_savestate` / `mds_loadstate` | `save_state(path)` / `load_state(path)` |
| `mds_cheat_add` | `add_cheat(code, name="", enabled=True)` -> id; `code` is the text or a list of lines |
| `mds_cheat_remove` / `_enable` / `_clear` | `remove_cheat(id)` / `enable_cheat(id, enabled=True)` / `clear_cheats()` -> count |
| `mds_cheat_list` + `mds_cheat_get` | `cheats()` -> list of `{"id", "name", "enabled", "words", "code"}` (`code`: the text, normalised); `cheat(id)` -> one |
| `mds_lua_run` / `_exec` / `_stop` | `lua_run(source, is_file=False)` / `lua_exec(code)` (returns the output) / `lua_stop()` |
| `mds_lua_output` | `lua_output()` -> str |
| `mds_packets` / `_take` | `packet_log(enabled=True, capacity=4096)` / `take_packets()` -> list of dicts (`"size"` added for a packet cut short, level 6) |
| `mds_debug_packet` | not wrapped (`native.lib().mds_debug_packet(...)`; argtypes are set) |
| `mds_serve` / `mds_serve_host` (+ `mds_serve_name`) | `native.serve(port=0, port_range=0, name=None, host=None)` -> bound port (`host` given: `mds_serve_host`) |
| `mds_serve_stop` / `mds_serve_wait` | `native.serve_stop()` / `native.serve_wait(timeout_ms=0)` |
| `mds_serve_name` | via `serve(name=...)` |
| `mds_set_instance_base` / `mds_instance_base` | `native.set_instance_base(base)` / `native.instance_base()` |
| `mds_mac` / `mds_save_path` | `mac()` -> 6 bytes / `save_path()` -> str |
| level 7 (`melonds_probes.h`) | see [section 5b, Python](#python) |

`mds_lua_run` raises for an error in the script's first step (API level 2).

---

## 10. Recipes

All recipes use `melonds_mcp.native`; the dq9-re copy works the same.
Paths are examples. Keep one console per process.

### Play inputs and take a screenshot

```python
import shutil
from datetime import datetime
from PIL import Image
from melonds_mcp import native

shutil.copy("saves/game.sav", "work/game.sav")          # melonDS writes to the save it boots
c = native.Console(bios9="bios9.bin", bios7="bios7.bin", firmware="firmware.bin")
c.load_rom("game.nds", save="work/game.sav")
c.set_rtc(datetime(2026, 1, 1, 12, 0, 0))               # games that read the clock replay only with the same clock

seq = [["A"]] * 4 + [[]] * 20 + [["Down"]] * 30          # one entry per frame
for keys in seq:
    c.submit_input(keys)                                 # queue holds 600; run in chunks for longer sequences
timed_out, states = c.run(len(seq))
assert not timed_out and states == [native.IDLE]

px = bytes(c.screen())                                   # 256x384, top screen first, BGRA
Image.frombuffer("RGBA", (256, 384), px, "raw", "BGRA", 0, 1).convert("RGB").save("shot.png")
c.close()
```

With the setters instead of the queue: `c.set_keys(keys); c.run(1)` per frame.

### Branch from a state

```python
c.save_state("work/base.mst")                            # console must be idle
results = {}
for name, first in (("a", ["A"]), ("b", ["B"])):
    c.load_state("work/base.mst")                        # also rewrites the save file from the state
    c.set_rtc(datetime(2026, 1, 1, 12, 0, 0))            # the state carries its own clock
    c.submit_input(first)                                # without it, the keys held at the save repeat (14.2)
    c.submit_input([])
    c.run(120)
    results[name] = c.read(0x020F6D48, 4)                # e.g. gold
c.run(2)                                                 # before trusting screen() after a load
```

### Find who writes an address

```python
c.track_calls(True)                    # enable early: frames from before this are unknown
wp = c.add_watchpoint(0x020F6D48, 4, write=True)
timed_out, (st,) = c.run(600)
if st == native.PARKED:
    b = c.take_break()                 # kind "write"; b.pc = the store; b.value = new value
    who = f"DMA channel {b.dma}" if b.dma >= 0 else f"{b.pc:#x}"     # a DMA write has no pc (b.pc = DMA tag)
    print(f"write {b.value:#x} to {b.addr:#x} by {who} (cpu {b.cpu})")
    for f in c.callstack(b.cpu):       # innermost first
        print(f"  {f['entry']:#010x} called from {f['site']:#010x}{' (exception)' if f['exception'] else ''}")
c.remove_point(wp)
c.run(0)                               # from parked: finish the frame and go idle
print(c.last_writer(0x020F6D48))      # same question after the fact (needs track_writers(True); raises if off)
```

### Log a function's arguments at each call

From level 7 a probe does this without parking: `c.add_probe(0x020D8B94, reads=[("r1", 64)])`, then
`c.take_probes()` after the run (the paths in `"reads"`, the results in `"ret"`; [section 5b](#function-probes)).
With breakpoints:

```python
bp = c.add_breakpoint(0x020D8B94)      # e.g. the file-open function
calls = []
end = c.frames + 600
while c.frames < end:
    timed_out, (st,) = c.run(end - c.frames)   # from parked, run(n) restarts the budget; the partial frame counts
    if st != native.PARKED:
        break
    b = c.take_break()
    if b.kind == "exec" and b.id == bp:
        r = c.regs(9)                  # exact for the CPU that broke
        name = c.read(r["r1"], 64).split(b"\0")[0]
        calls.append((c.frames, r["r0"], name, r["lr"]))
    elif b.kind == "request":
        continue                       # an interrupt (level 6: never a leftover one); just resume
c.remove_point(bp)
if c.state == native.PARKED:
    c.run(0)
```

### Collect call counts and coverage for a diff

```python
def profile(state, inputs, frames):
    c.load_state(state)
    c.take_call_counts(9); c.take_coverage(9)   # drop leftovers: loads do not clear them
    c.track_calls(True); c.coverage(True)
    for keys in inputs:
        c.submit_input(keys)
    c.run(frames)
    counts, cov = c.take_call_counts(9), set(c.take_coverage(9))
    c.track_calls(False); c.coverage(False)
    return counts, cov

ca, va = profile("work/base.mst", [["A"]] + [[]] * 59, 60)
cb, vb = profile("work/base.mst", [["B"]] + [[]] * 59, 60)
only_b = {f: n for f, n in cb.items() if f not in ca}        # functions called only in B
changed = {f: (ca[f], cb[f]) for f in ca.keys() & cb.keys() if ca[f] != cb[f]}
new_code = sorted(vb - va)                                   # instructions run only in B (catches tail calls, jump tables)
```

### Apply an Action Replay code after loading a state

```python
c.load_state("work/field.mst")                  # a load clears the cheat list
gold = c.add_cheat("020F6D48 0098967F", "max gold")
c.run(2)                                        # the code runs at the ARM7's VBlank IRQ in each frame
c.enable_cheat(gold, False)                     # stops writing; what it wrote stays
print(c.cheats())
```

### Read an input log

```python
for e in c.input_log(from_frame=0, max=100000):
    keys = [k for i, k in enumerate(native.KEYS) if e["buttons"] >> i & 1]
    print(e["frame"], e["seq"], keys, "lid" if e["lid"] else "")
# Only consumed payloads are logged: a gap in "frame" means the empty-queue rule applied
# (repeat: the previous state was held).
```

---

## 11. Known issues and surprises

Bugs (not fixed here) and behaviour that is easy to get wrong. *(verified)* items were reproduced
with one console against this branch's DLL. Items fixed in `fix/api-quirks-2` (level 6) and in
`fix/api-quirks` (level 2) are listed after the open ones, with what changed; the open items were
renumbered each time.

1. **The debugger cannot read or write I/O**: `mds_read` (and Lua `memory.read_*`) return 0 for
   `0x04xxxxxx` *(verified)*, and `mds_write` refuses it (`I/O at 0x... cannot be written by the
   debugger`), so register reads and writes, which have side effects, never happen behind the
   game's back. Read I/O from guest code or a Lua hook that runs in the game.
2. **`mds_input_log` `touch_count` is never 0**: a payload without touch points logs 1 (the held
   pen point is counted), unlike the payload header where 0 means "unchanged". The C API cannot
   return the logged touch points or mic samples.
3. **Lua `savestate.save`/`load` fail inside a running script or hook**: the console is `Running`
   at frame end and inside hooks, and states need an idle console. They only work from
   `mds_lua_exec` or a script body's first step while idle. *(inferred)*
4. **`mds_loadstate` rewrites the save file** from the state's save memory. Deliberate and kept for
   now (see [section 6](#6-savestates)): a save booted in place is overwritten by the first load,
   so clients must boot a copy (the dq9-re harness does).
5. **Loads leave the process counters alone**: `mds_frames` and the input log do not jump on a
   load (the emulated frame that seeds the mic noise does; see [section 3](#microphone-fifo)).
   States older than 14.2 restore no input: after loading one, the keys and pen held before the
   load repeat until a payload arrives.
6. **Serving changes in-process behaviour**: `mds_serve` and each client connect/disconnect reset
   every console's input mode to repeat (stall 15); a connected client takes break events (reset
   events included) and Lua output from in-process callers; a disconnect clears every console's
   input queue.
7. **Barriers drop consoles that stop**: a console whose frame budget runs out leaves every barrier
   group it is in, including groups for payloads still queued. Running the members one frame at a
   time therefore never synchronises them (intended per the tests, but surprising).
8. **`mds_stop_after(id, 0)` and `(id, 1)` are the same** while a frame is in progress, and so are
   `mds_start(id, 0)` and `(id, 1)` on a parked console *(verified for start 0)*.
9. **A DMA break parks inside the DMA**: between two units, with the CPUs between instructions,
   once per transfer.
   Commands work as at any park, but the console is not at a frame boundary (no savestates), and
   the program that started the DMA is not recorded (the event's `pc` is the DMA tag; `mds_get_regs`
   gives each CPU's next instruction).
10. **Savestates 14.2 do not load in older builds** (upstream melonDS and builds before this
    branch reject them as "state from the future"); this build loads 14.0-14.2.
11. **The dq9-re playbook lags behind the API** (`docs/playbook.md`, "The emulator interfaces" and
    "How the emulator was changed"). The four points this item named at level 5 are corrected
    there (`mds_set_local_dir` is not the BIOS/firmware folder; coverage is per instruction; calls
    return counts and ids as well as 0; the server serves one client at a time). It still points
    to `merge/ipc` for this reference, lists no functions of levels 2-6 beyond the cheat calls and
    describes the history in rounds that stop at level 3. The fix belongs in dq9-re.
12. **The call tracker is heuristic** (level 4): besides `BL`/`BLX` it pushes frames for jumps made
    with the return address in LR and for functions that start with `PUSH {..., LR}`, and it seeds
    the frames below the first call from stack memory by scanning for prologues. Hand-written code
    that saves LR without being called can add a frame, a non-standard prologue can stop the seeding
    early, and call counts are higher than level 3 gave for the same run (they include those calls).
13. **A PC change at a break re-runs the hooks there** (level 4, `step_redirect`): with a breakpoint
    at the new PC, the resume parks again at once (kind exec) before running anything. Resume again
    to go on.
14. **`memory.execute` drops the callee's return value**: a call made with it returns with r0-r3,
    r12, LR and the flags as they were before. It works only inside an exec hook or at a break of
    the same CPU, never from a read/write hook or from `mds_lua_exec` on an idle console.
15. **Lua `wifi` hooks and injected frames change the run**: a hook that drops or replaces frames,
    or `wifi.inject`, alters what the console (and its partners) receive, so such runs are not
    deterministic replicas of runs without them. A hook that only looks returns nothing (or `true`)
    and changes nothing.
16. **No thread stacks over the socket protocol or the CLI**: `mds_threadstacks` and Lua
    `debugger.threadstacks` have no wire message yet; the MCP server uses the native library and has
    them (`callstack(threads=true)`).
17. **The headless symbol loader reads local files only**; a URL (which the Qt debugger accepts)
    fails with "cannot download URL".
18. **Overlay detection samples memory** (`mds_overlays`, `monitor overlays`, symbol tables): an
    overlay whose code was partly overwritten, or patched by more than 2 of the 8 sampled windows,
    is reported as not loaded, and one whose sampled bytes happen to match another overlay's code
    at the same address can be reported as loaded.
19. **`src/debug/gdb_test` does not build**: upstream's mock stub predates the current `GdbStub`
    constructor and `Init` signatures. The GDB behaviour is tested over a socket instead
    (`test_gdb_overlays.py`, `test_api_fixes2.py`).
20. **A GDB restart is still not replayable**: it is reported now (a reset event and
    `mds_reset_count`), but a recorded run does not replay across it, and `mds_frames`, the input
    queue and the input log keep counting through it.
21. **Every park drops every break request** (level 6): a Lua `debugger.pause()` for one CPU that has
    not fired yet when the console parks for another reason (a breakpoint on the other CPU, an
    interrupt) is dropped with the rest. Armed step counts are kept.
22. **Level 7 is C API and Python only**: probes, the memory log, actions and overlay tags have no socket
    protocol messages, CLI commands, MCP tools or Lua functions yet.
23. **A probe's return is a heuristic** (level 7): the first time the CPU reaches the return address with the
    entry's SP and mode. A function that leaves by longjmp or never returns stays awaited (256 per CPU at most,
    then the oldest is given up), and one entered by a jump whose LR is not its return address is matched at
    wherever LR points, if ever. A state load gives up every awaited return.
24. **Memory log reads show memory as the access starts** (level 7): the value is read through the debugger,
    so I/O reads log 0.
25. **A moving action hides a breakpoint at its address** (level 7): when a return, skip or set-r15 action fires,
    the instruction at its address does not run and a breakpoint there does not fire (one at the new PC does).
    Actions that do not move PC leave breakpoints alone.
26. **Overlay attribution reads memory, not the game's tables** (level 7): code an unload left in memory is
    still attributed to its overlay (that is the code there), an overlay with no code (all bss) is
    `MDS_OVERLAY_UNKNOWN`, and two overlays with identical code at an address and at their starts are
    `MDS_OVERLAY_AMBIGUOUS`.

### Fixed in fix/savestate-irq

Not items of the level-6 list (found while pooling consoles in dq9-re); the tests are in
`tools/melonds-mcp/tests/test_savestate_reuse.py`.

- **A load kept CPU state from before it**: the interrupt lines and the ARM9 code-fetch timing were
  not recomputed, so a console that had run something before a load ran its first instructions
  differently from a fresh console (and a fresh one differently from the run that saved the state).
  Both are derived from the loaded state now ([section 6](#6-savestates); feature
  `savestate_derived`).
- **States held uninitialised bytes** in the GX FIFO entries' padding (feature `gx_fifo_zeroed`).

### Fixed in fix/api-quirks-2

These were items of the level-5 list (old numbers in brackets); the tests are in
`tools/melonds-mcp/tests/test_api_fixes2.py`.

- **`mds_interrupt` left a break request armed on the other CPU** [1]: every park, and the end of
  a run, drop the break requests of both CPUs (armed steps stay), so the resume after an interrupt
  runs; the consoles of a group that `mds_wait`/`mds_run` interrupt are cleaned the same way.
  `mds_interrupt` ignores a stopped console, whose next run it used to park at once
  ([Running](#running); feature `interrupt_clean`).
- **`mds_get_regs` pc was wrong while parked** [2], and **request breaks served at a frame boundary
  reported the raw ARM9 R15** [11]: index 15 is the instruction each CPU runs next (`R15 - 4` / `- 2`)
  at every park and for both CPUs, so neither the CPU without hooks (which read 0) nor the armed one
  that did not break (one behind) is wrong any more; at a fault it is the faulting instruction. A
  frame-boundary request reports the ARM9's next instruction as `pc`. `r15_raw` is unchanged; the
  pc at a DMA break is documented ([Registers](#registers), [Breaks](#breaks-what-happens-and-mds_take_break);
  feature `pc_next`).
- **`mds_write` silently dropped palette, VRAM, OAM and GBA ROM area writes on the ARM9 bus** [3]:
  palette, VRAM and OAM are written with halfwords (odd edges merged), every write is read back, and
  what cannot be written fails naming the region: I/O (never written), the GBA ROM area and the
  BIOSes (read-only), and writes that did not take (unmapped VRAM, a powered-off 2D engine, no GBA
  cart SRAM). Lua `memory.write*` in the headless build does the same and raises the error
  ([Reading and writing memory](#reading-and-writing-memory); feature `write_regions`). I/O reads
  are still 0 (open item 1).
- **`mds_packets_take` could get stuck on a packet longer than `max_data`** [5]: the first packet
  of a call is cut to `max_data` bytes, with its size in the new `mds_packet.total` (former
  padding), so a call never returns 0 while packets are staged
  ([Wireless packet capture](#wireless-packet-capture); feature `packets_truncate`).
- **`mds_frames` returned 0 for an unknown id; `mds_destroy` ignored unknown ids** [13]: `mds_frames`
  returns `MDS_FRAMES_ERROR` and `mds_destroy` (now `int`) returns -1, both with `no console N`
  (feature `unknown_id_errors`).
- **Lua memory hooks were plain points** [14]: they are tagged `MDS_POINT_LUA` in `mds_point_list`,
  and `mds_point_remove` refuses them and wifi hook ids (feature `point_lua_tag`).
- **`mds_point_list` had no total count** [15]: `mds_point_count` returns it; `mds_point_list`
  still returns the number copied ([Breakpoints and watchpoints](#breakpoints-and-watchpoints);
  feature `point_count`).
- **A GDB restart reset the console without telling the C API** [25]: `vRun`, `R`, `r` and
  `monitor reset` queue a reset event (kind 6, `MDS_BREAK_RESET`; not a park) that
  `mds_take_break` returns and the socket server forwards, and `mds_reset_count` counts it. What is
  still missing is open item 20 ([GDB stub](#gdb-stub); feature `gdb_reset_event`).

### Fixed in fix/api-quirks

These were items of the level-1 list (old numbers in brackets):

- **`debug = 0` failed silently** [13]: the debugger calls now fail with "console N was created
  without the debugger (debug = 0)" ([section 2](#creating-and-destroying)).
- **Savestates did not carry `KeyInput`; keys held before a load were applied after it; debugger
  data went stale over a load** [15]: savestates 14.2 carry `KeyInput`, the pen, the held input and
  the mic buffers; a load restores the held input and clears call stacks, a pending watchpoint hit
  and last-writer records ([section 6](#6-savestates), [section 5](#persistence-of-debugger-data)).
- **Mic level noise depended on process history** [14]: it is seeded from the emulated frame, which
  states carry ([section 3](#microphone-fifo)).
- **The GDB stub listened on all interfaces** [23]: it listens on 127.0.0.1 unless `mds_gdb_host`
  names another address; the socket server can listen elsewhere on request (`mds_serve_host`,
  `--host`) ([section 8](#8-gdb-stub-and-socket-server)).
- **Negative `max` crashed or misbehaved** [7]: every take/list call and `mds_run`/`mds_wait`'s
  count refuse it.
- **DMA watchpoint breaks reported the CPU's last instruction as `pc` and fired when that CPU next
  executed** [20]: they park at once with `pc` = the DMA tag and the channel in `dma`, once per DMA
  transfer (as the queued hit was; for a while each watched unit parked).
- **`mds_load_rom` accepted a parked console** [12]: it needs an idle one.
- **`mds_lua_run`/`exec`/`stop` ran on the calling thread, and a first-step runtime error returned
  0** [11]: they run on the emulator thread through the command queue, and the error fails the call.
- **`mds_step` from idle executed one instruction fewer than from parked** [3]: `n` executes `n`
  instructions from both ([Stepping](#stepping-mds_step)).
- **`mds_version` was always 1** [9]: it is the API level (2), with `mds_has` for features
  ([Version and features](#version-and-features)).
- **`mds_lastwriter_get` returned 0 for errors as well as "no record"; Lua did not decode DMA
  writers** [6]: `mds_lastwriter_query` separates them (the old call stays as legacy) and Lua
  returns `nil, cpu, channel` for a DMA write ([Last writer](#last-writer)).
