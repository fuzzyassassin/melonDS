# Debugger (Qt frontend)

The Debug menu opens the tools. All of them work on a running or stopped console.

Breakpoints, watchpoints, stepping, tracing, call stacks and coverage need the interpreter: turn
JIT off in Config > Emu settings. With JIT on, the viewers still work and the debugger window says
why the rest is unavailable.

Building needs capstone for disassembly (`pacman -S mingw-w64-ucrt-x86_64-capstone`,
`apt install libcapstone-dev`, `brew install capstone`, or vcpkg). Without it the debugger shows
raw opcodes.

## Symbols

Symbols... in the debugger window's toolbar loads a dsd `config.yaml` (or the folder holding it),
or a text file of `address name [size]` lines. Its arrow also takes a URL: a raw or github.com
link to a `config.yaml` or its folder, such as
`https://github.com/OWNER/REPO/blob/BRANCH/config/usa/arm9/config.yaml`; nothing needs to be
checked out. The choice is remembered per game code and shared by every tool.

- Overlays are detected by comparing RAM with the overlays in the running ROM, decompressed, so no
  extract is needed; only loaded overlays' names are shown.
- Labels... layers a text file of `address name [module]` lines (`#` starts a comment) over the
  loaded symbols, also remembered per game code. A label renames the symbol that starts at that
  address in that module (`main`, an autoload or `ovNNN`; default: the static module covering
  the address) or adds one; the old name still works in Go to. Overlay labels show only while
  the overlay is loaded. Cancel the dialog to remove the labels.
- C++ names are demangled (Demangle toggles it). Go to, breakpoint and watch fields accept an
  address, a symbol in either form, or `symbol+0x10`.

## Source

The Source dock shows the source of the function at the selected line of the listing, with the
function highlighted: C, C++ or assembly, from the file that the config's `delinks.txt` assigns
to that address. It follows the selection, so a click in the listing or a double-click in the call
stack shows that function. Files come from beside the config, or from the same repository when the
symbols came from a URL; downloads are cached. Open opens a local file in its editor, or a GitHub
file in the browser at those lines. A function that is not in any delinked file has no source yet.

With line info the dock marks the exact line of the selected address, marks the line the stopped
CPU is on, and a click in its margin toggles a breakpoint on that line. Line info is the DWARF
`.debug_line` of an ELF linked with `-sym on` (mwldarm; the ROM bytes do not change). For a local
`config.yaml` it loads by itself from `build/<region>/arm9.o` of that decomp; Symbols... > Line
info from an ELF... loads any other one. Rows are matched to modules through `delinks.txt`, so
overlays that share addresses resolve to the loaded one. When that ELF is rebuilt, the symbols, line
info and source files reload a second after the linker finishes writing it.

## Starting from the command line

    melonDS --symbols config.yaml --labels game.labels --state scene.mln --break 0x02087d78 --break Foo_Run --pause game.nds

Once the ROM has booted, melonDS pauses, loads the symbols and labels (as Symbols... and Labels... do, so they are
remembered for the game), loads the savestate, adds an ARM9 execute breakpoint for each `--break` (an address, a
symbol or label, or `symbol+0x10`; read after the symbols and labels), then runs again unless `--pause` is given.
`--debugger` opens the debugger window; a breakpoint hit opens it anyway. A missing file or an unknown name is logged
and shown on screen, and the rest still applies. Breakpoints are by address: on an overlay function they mean that
function while its overlay is the one loaded (the savestate's). They need JIT off. Test: `tests/test_qt_startup.py` in
`tools/melonds-mcp` (needs a `-DMELONDS_AUTOMATION=ON` build named by `MELONDS_QT_AUTOMATION_EXE`).

## Debugger window

| key | does |
|---|---|
| F6 | break on the selected CPU |
| F5 | continue (also unpauses) |
| F11, F10, Shift+F11 | step into, step over, step out |
| Ctrl+F10 | run to the next branch or return in this function, stepping over calls |
| Ctrl+F11 | run until a branch, call or return actually jumps; stop at its target |
| F4 | run to the selected line |
| F9, click in the margin | toggle a breakpoint |
| Ctrl+G | go to address or symbol |
| Enter, double-click | follow a branch or literal |
| Backspace | back |

Keys... changes the run and step keys (F4 to Ctrl+F11 above). While the debugger is open they also
work in the game window, ahead of its own keys such as F1-F8 for loading states; Keys... turns that
off.

Step over (F10) steps over BL, BLX, SWI and calls made through `mov lr, pc` with `bx` or `ldr pc`,
and steps through every other branch. Instructions are coloured by kind, in the disassembly and the
trace: calls orange, returns red, branches purple, loads cyan, stores green, compares yellow,
system and coprocessor blue, literal data dimmed, everything else plain.

Double-click a register to change it while stopped. The docks hold both CPUs' registers,
breakpoints and watchpoints (CPU and DMA writes, enable, hits, owner) and the call stack. A Lua
hook that returns true opens the window at the stop.

The call stack is tracked in the core, per thread: BL/BLX, calls through `mov lr, pc`, tail calls,
interrupts and SWIs, and thread switches (by stack pointer, including NitroSDK's context loads).
Threads that are not running are listed under the current one. Frames that were already on the
stack when tracking started or a savestate loaded are read back from stack memory at the next call.

The Arguments column shows each tracked call's arguments as they were at the call: r0-r3, then the
first four stack words. A C++ symbol's demangled signature gives their types: `this` for a member
(static members are shown with one too), numbers by type, `char*` and `const char*` as strings.
Without a signature it shows r0-r3, with any that point at text as strings. The tooltip lists every
argument with its register and type and the full string; double-click or right-click opens a
pointer argument in the memory viewer. Frames read back from stack memory have no arguments.

## Tools

| tool | shows |
|---|---|
| Trace | ring buffer of executed instructions in a PC range, savable |
| Call counts and coverage | calls per function, executed instructions per function with % covered |
| Last writer | the instruction or DMA channel behind the latest write to each main-RAM word |
| RAM search, RAM watch | DeSmuME-style search with autosearch; typed watches (fx32 too), separators, recent files, auto-load per game; Add cheat turns a result or watch into an Action Replay code |
| IO registers | ARM9 and ARM7 registers decoded to fields, editable |
| Palettes, Tile viewer, BG map, OAM / sprites | both engines, extended palettes, all BG modes |
| 3D graphics | last frame's polygons and vertices, wireframe with picking, textures in all formats |
| Matrices | projection, position, direction, texture and clip matrices, every stack slot, decimal or hex |
| Lights | four lights, material colours, shininess table, current vertex colour and normal |
| Sound | 16 channels and both capture units, live levels, mute and solo per channel, capture analysis |
| NitroFS | the ROM's file tree and overlay tables, loaded overlays, extract, view in the memory viewer |
| Log | game output through the no$gba debug registers, and the emulator log |
| Memory viewer | #2384's viewer plus the ARM7 bus, cart ROM and firmware; symbols, last writer, value decoding, break on read/write, text dump |
| Lua console | scripts plus a command line; several script windows, recent scripts, restart, reload on change |

The debugger window steps N instructions at a time, refreshes the listing while running, follows
the PC and forces ARM or Thumb decoding.

## Debug menu

| item | does |
|---|---|
| Movie | record from power-on or from now, play, stop, read-only, rerecord count |
| Record | AVI (MJPEG or uncompressed, split at 1.9 GB), WAV, screenshot |
| Boot with NitroFS overrides | reboots the cart with files from a folder replacing the ROM's (`data/...` paths, any size) |
| View layers | hide BG0 to BG3 and OBJ per engine |
| HUD | FPS, input, graphical input, frame counter, lag counter, RTC, microphone |

Movies are DeSmuME `.dsm` text files with the start state embedded. Saving a state while a movie
runs writes `<state>.movie` beside it; loading that state seeks the movie in read-only mode and
truncates it and counts a rerecord in read-write mode. A state from another movie stops it. Record
with JIT off when you rerecord.

## Lua

The Qt console has #1671's API plus the debugger tables from the headless build: `event.onmemoryexecute`,
`onmemoryread`, `onmemorywrite`, `onframeend`, `emu.getregister`, `setregister`, `getregisters` and
`debugger.*` (`callstack`, `threadstacks`, `trackcalls`, `callcounts`, `coverage`, `trace`, `trackwriters`,
`lastwriter`, `pause`, `step`, `resume`, `peek`). A `callstack` frame of a tracked call carries
`args`, r0-r3 at the call.

`wifi.hook(fn, name)` installs a local wireless driver: `fn` sees every frame the console sends or
receives (MP commands, replies and acks, beacons, management and data frames, local play and
Nintendo WFC alike) as `{tx, data, rate, channel, time}`, where `data` is the 802.11 frame with its
FCS. Return `false` to drop the frame or a string to replace it. `wifi.inject(data, rate)` delivers a
frame as if it had been received. `wifi.decode(data)` returns the 802.11 header (type, subtype,
flags, addresses, sequence), the Nintendo MP fields (`kind` `mpcmd`/`mpreply`/`mpack`, client
mask and time, command count, failed clients, payload), WEP IV and ICV, LLC/SNAP and ethertype for
data frames, and fixed fields and elements (SSID, channel, the Nintendo vendor element) for
management frames. Wifi hooks also run with JIT on; remove them with `event.unregisterbyid` or
`unregisterbyname`.

Memory hooks run on the emulator thread at the instruction that hit them, so `emu.getregister`
there is exact. `gui`, `forms`, `input`, `client`, `savestate` and `console` are refused inside a
hook: set a variable there and act on it in `_Update`.

In an exec hook, `memory.execute(op, ...)` (opcodes as numbers or one table, ARM or Thumb by the
current state, a Thumb BL as one 32-bit value) runs those instructions as if they sat in front of
the hooked one, with PC reading as it would there. It returns `true` when one of them branched: the
hooked instruction is skipped and execution continues at the target, so `memory.execute(0xE12FFF1E)`
(`bx lr`, `0x4770` in Thumb) at a function's entry makes it return at once. A call (`bl`, `blx`,
`swi`) returns to the hooked instruction with r0-r3, r12, lr and the flags as they were before
`memory.execute`, and that return does not fire the instruction's hooks again, so a hook can call a
function on every hit without a guard; the callee's return value is not kept. Otherwise the hooked
instruction runs next. Code written
from a hook with `memory.write*` takes effect on the instruction about to run, and setting `pc` or
the T bit with `emu.setregister` or DeSmuME's `memory.setregister` jumps there. It needs JIT off.

DeSmuME scripts run unchanged: `emu` (`frameadvance`, `registerbefore`, `registerafter`,
`emulateframe`, `framecount`, `lagcount`, `lagged`, ...), `memory` (`readbyte` to `readdwordsigned`,
`readbyterange`, `write*`, `getregister`, `setregister`, `registerwrite`, `registerread`,
`registerexec`), `gui` (`text`, `box`, `line`, `pixel`, `getpixel`, `opacity`, `transparency`,
`gdscreenshot`, `gdoverlay`, `register`, `setlayermask`, `popup`), `joypad`, `stylus`, `input`,
`savestate` (slots, files and in-memory objects with `registersave`/`registerload`), `movie`,
`sound`, `bit` and the `AND`/`OR`/`XOR`/`SHIFT`/`BIT` globals. The script body runs as a coroutine
on the emulator thread at frame boundaries, so `emu.frameadvance` and `joypad.set` act on exact
frames. Coordinates follow DeSmuME: y from -192 to -1 is the top screen.

## Threading

`debugger/DebugBridge` is the one channel to the emulator thread. `post` and `call` queue work
that runs at the next instruction (interpreter), the next frame (JIT), while paused, or while
stopped at a break; `call` hands the result back on the GUI thread. Nothing on the GUI thread
waits for the emulator thread except `run`, and the emulator thread never waits for the GUI, so
neither side can deadlock the other. Saving or loading a state, reset and similar commands sent
while stopped let the current frame finish first.

The same debugger without a window is the headless library; its C API (`melonds_api.h`) is
documented in [docs/headless-api.md](docs/headless-api.md).

`-DMELONDS_AUTOMATION=ON` builds a file-driven command interface (`MELONDS_AUTOMATION=<dir>`) used
to test these windows without a display.
