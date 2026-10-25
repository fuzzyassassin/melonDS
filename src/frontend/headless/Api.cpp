/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "melonds_api.h"
#include "ApiInternal.h"
#include "Cheats.h"
#include "GPU_Soft.h"
#include "Instance.h"
#include "LuaHost.h"
#include "MPInterface.h"
#include "Server.h"
#include "Socket.h"
#include "debug/RomOverlays.h"

using namespace melonDS;
using namespace Headless;

namespace
{

constexpr int MaxInstances = 16;

struct Slot
{
    std::unique_ptr<Instance> Inst;
    std::vector<TraceEntry> Trace;
    std::vector<std::pair<u32, u64>> Calls[2];
    std::vector<u32> Coverage[2];
    std::string LuaOutput;
    std::vector<PacketRecord> Packets;
};

std::mutex SlotsLock;
// Never destroyed: at process exit the loader lock is held and the emulator threads are already gone, so
// running ~Instance (which joins its thread) from a static destructor would hang the exit.
std::map<int, Slot>& Slots = *new std::map<int, Slot>();
thread_local std::string LastError;

bool Level1Function(const std::string& name)
{
    static const char* const Names[] = {
        "mds_version", "mds_error", "mds_set_local_dir", "mds_set_log_level", "mds_mp_timeout", "mds_set_pacing",
        "mds_create", "mds_destroy", "mds_load_rom", "mds_reset", "mds_set_rtc", "mds_get_rtc", "mds_run",
        "mds_wait", "mds_gdb", "mds_start", "mds_stop_after", "mds_interrupt", "mds_state", "mds_take_break",
        "mds_frames", "mds_screen", "mds_set_keys", "mds_touch", "mds_set_lid", "mds_input_submit",
        "mds_input_submit_multi", "mds_debug_recv_pending", "mds_input_mode", "mds_input_depth",
        "mds_input_waiting", "mds_input_clear", "mds_input_log", "mds_read", "mds_write", "mds_get_regs",
        "mds_set_reg", "mds_bp_add", "mds_wp_add", "mds_point_remove", "mds_point_enable", "mds_point_list",
        "mds_step", "mds_trace", "mds_trace_take", "mds_calls", "mds_callstack", "mds_callcounts_take",
        "mds_coverage", "mds_coverage_take", "mds_lastwriter", "mds_lastwriter_get", "mds_savestate",
        "mds_loadstate", "mds_lua_run", "mds_lua_exec", "mds_lua_stop", "mds_lua_output", "mds_packets",
        "mds_packets_take", "mds_serve", "mds_serve_stop", "mds_serve_wait", "mds_serve_name",
        "mds_set_instance_base", "mds_instance_base", "mds_mac", "mds_save_path",
    };
    for (const char* n : Names)
        if (name == n)
            return true;
    return false;
}
bool MPReady = false;

std::mutex HookLock;
std::function<void(Instance&)> CreatedHook;
std::function<void(int)> DestroyedHook;

int Fail(const std::string& error)
{
    LastError = error;
    return -1;
}

Slot* Find(int id)
{
    std::lock_guard lock(SlotsLock);
    auto it = Slots.find(id);
    if (it == Slots.end())
    {
        LastError = "no console " + std::to_string(id);
        return nullptr;
    }
    return &it->second;
}

Slot* Stopped(int id)
{
    Slot* s = Find(id);
    if (s && s->Inst->GetState() == RunState::Running)
    {
        LastError = "console " + std::to_string(id) + " is running";
        return nullptr;
    }
    return s;
}

// The console must have been created with the debugger (debug != 0): without it the CPUs run without the
// debugger hooks, so breakpoints, watchpoints, steps, traces, call tracking, coverage and CPU last-writer
// records would silently never happen.
Slot* Debug(Slot* s, int id)
{
    if (s && !s->Inst->DebugEnabled())
    {
        LastError = "console " + std::to_string(id) + " was created without the debugger (debug = 0)";
        return nullptr;
    }
    return s;
}

Slot* StoppedDebug(int id)
{
    return Debug(Stopped(id), id);
}

Slot* Idle(int id, const char* what)
{
    Slot* s = Find(id);
    if (s && s->Inst->GetState() != RunState::Idle)
    {
        LastError = what;
        return nullptr;
    }
    return s;
}

// Buffer sizes and counts: negative ones are refused before anything is copied or allocated.
bool CountOk(int n, const char* name)
{
    if (n >= 0)
        return true;
    LastError = std::string(name) + " must not be negative";
    return false;
}

int StateCode(RunState st)
{
    switch (st)
    {
    case RunState::Running: return MDS_RUNNING;
    case RunState::Parked: return MDS_PARKED;
    default: return MDS_IDLE;
    }
}

ARM& CPU(Instance& inst, int cpu)
{
    return cpu ? (ARM&)*inst.NDS->ARM7 : (ARM&)*inst.NDS->ARM9;
}

u32 SPSR(ARM& arm)
{
    switch (arm.CPSR & 0x1F)
    {
    case 0x11: return arm.R_FIQ[7];
    case 0x12: return arm.R_IRQ[2];
    case 0x13: return arm.R_SVC[2];
    case 0x17: return arm.R_ABT[2];
    case 0x1B: return arm.R_UND[2];
    default: return 0;
    }
}

// Run f(inst, error) on the console's emulator thread and return its result. A negative
// result with a non-empty error reports that error to the caller's thread.
template <typename F>
int OnEmu(Slot* s, F f)
{
    int result = -1;
    std::string error;
    if (!s->Inst->Call([&](Instance& inst) { result = f(inst, error); }))
        return Fail("console is shutting down");
    if (result < 0 && !error.empty())
        LastError = error;
    return result;
}

template <typename T, typename F>
int Take(std::vector<T>& staging, F refill, T* out, int max)
{
    if (staging.empty())
        staging = refill();
    int n = std::min<int>(max, staging.size());
    std::copy(staging.begin(), staging.begin() + n, out);
    staging.erase(staging.begin(), staging.begin() + n);
    return n;
}

}

extern "C" {

int mds_version(void)
{
    return MDS_API_VERSION;
}

int mds_has(const char* name)
{
    // Features of API level 2 (fix/api-quirks); see docs/headless-api.md, section 1.
    static const char* const Features[] = {
        "api_version",          // mds_version reports the API level; this function
        "debug_off_errors",     // debugger calls fail on a console created with debug = 0
        "savestate_input",      // savestates 14.2 carry KeyInput, the held input and the pen (load restores them)
        "savestate_debug_reset",// a state load clears call stacks, a pending watch hit and last-writer records
        "mic_noise_emulated",   // level-mic noise is seeded from the emulated frame, which states carry
        "serve_host",           // mds_serve_host, melonds_server --host
        "gdb_host",             // mds_gdb_host; the GDB stub listens on 127.0.0.1 by default
        "count_checks",         // negative max/count arguments fail
        "dma_break",            // DMA watchpoint breaks park at once, pc = the DMA tag
        "load_rom_idle",        // mds_load_rom refuses a parked console
        "lua_queue",            // Lua run/exec/stop go through the command queue; a first-step error fails the call
        "step_exact",           // mds_step(n) executes n instructions from idle and from parked
        "lastwriter_query",     // mds_lastwriter_query
        // API level 3 (feature/cheats)
        "cheats",               // mds_cheat_*: Action Replay codes run by the core's engine every frame
        // API level 4 (merge/upstream-debug: ZevyaDev's debug branch)
        "thread_stacks",        // mds_threadstacks, Lua debugger.threadstacks: the stacks of threads not running
        "callstack_unwind",     // call stacks seed frames from stack memory, follow non-BL calls and NitroSDK switches
        "step_redirect",        // a PC change at a break or by a hook re-runs the hooks there; mds_step counts from it
        "pipeline_refetch",     // writes at a break or from a hook to the instructions being fetched take effect
        "lua_execute",          // Lua memory.execute in exec hooks and at breaks
        "lua_wifi",             // Lua wifi.hook, wifi.inject, wifi.decode
        "symbols_rom",          // the server's symbols_load takes overlay images from the running ROM; delinks
        // API level 5 (merge/upstream-debug-2)
        "rom_overlays",         // mds_overlays: the resident ARM9 overlays, detected in the core (RomOverlays)
        "gdb_monitor_overlays", // the GDB stub answers "monitor overlays" (qRcmd output)
        "gdb_session_reset",    // a GDB disconnect (also EOF) clears the stub's breakpoints, no-ack and extended mode
        "gdb_extended",         // GDB extended mode (!): vKill keeps the connection, vRun restarts (direct boot kept)
        // API level 6 (fix/api-quirks-2)
        "interrupt_clean",      // a park and the end of a run drop leftover break requests; mds_interrupt ignores a stopped console
        "pc_next",              // mds_get_regs pc = each CPU's next instruction at every park; frame-boundary requests too
        "write_regions",        // mds_write/Lua writes: halfwords for palette/VRAM/OAM, read back, errors name the region
        "packets_truncate",     // mds_packets_take cuts a first packet longer than max_data short (mds_packet.total)
        "unknown_id_errors",    // mds_frames returns MDS_FRAMES_ERROR and mds_destroy -1 for an unknown id
        "point_lua_tag",        // mds_point_list flags MDS_POINT_LUA on Lua hooks; mds_point_remove refuses them
        "point_count",          // mds_point_count: the total mds_point_list has to list
        "gdb_reset_event",      // a GDB restart queues an MDS_BREAK_RESET event (socket: a break event); mds_reset_count
        // API level 8 (feature/render-control)
        "render_skip",          // mds_set_render: skip 2D / 3D drawing; capture-safe; applied at the next frame boundary
        "threaded_3d",          // mds_set_threaded_3d: the soft 3D renderer on its own thread
        "render_stats",         // mds_render_stats: the settings in force and the skip counters
        // API level 9 (fix/savestate-irq)
        "savestate_derived",    // a load recomputes the CPUs' IRQ lines and the ARM9 code timing: nothing of the run before it survives
        "gx_fifo_zeroed",       // GX FIFO entries are written with zeroed padding, so states hold no stray bytes
        // API level 10 (feature/api-level-10: names for the review fixes of 2026-10-22, made at level 9)
        "dma_break_once",       // a DMA watchpoint parks once per transfer, not once per unit
        "dma_hook_old_value",   // DMA write hooks run before the unit is written: they see the old value
        "dma_break_patch",      // code patched at a DMA break runs (the break runs in the service context, pipelines refetched)
        "step_from_hook",       // step(n) from a breakpoint callback counts the hooked instruction: n instructions, not n + 1
    };
    static const char* const Functions[] = {
        "mds_has", "mds_serve_host", "mds_gdb_host", "mds_lastwriter_query",
        "mds_cheat_add", "mds_cheat_remove", "mds_cheat_enable", "mds_cheat_clear", "mds_cheat_list",
        "mds_cheat_get", "mds_threadstacks", "mds_overlays",
        "mds_debug_packet", "mds_point_count", "mds_reset_count",
        "mds_set_render", "mds_set_threaded_3d", "mds_render_stats",
    };
    if (!name)
        return 0;
    std::string n = name;
    for (const char* f : Features)
        if (n == f)
            return 1;
    for (const char* f : Functions)
        if (n == f)
            return 1;
    if (Headless::Api::ProbeFeature(n))    // probes, memlog, bp_actions, overlay_tags (ApiProbes.cpp)
        return 1;
    // every function of API level 1 is still exported
    return n.rfind("mds_", 0) == 0 && Level1Function(n) ? 1 : 0;
}

const char* mds_error(void)
{
    return LastError.c_str();
}

void mds_set_local_dir(const char* dir)
{
    SetLocalDirectory(dir ? dir : ".");
}

void mds_set_log_level(int level)
{
    SetLogThreshold((Platform::LogLevel)level);
}

void mds_set_pacing(int fps)
{
    SetPacing(fps);
}

int mds_mp_timeout(int ms)
{
    if (!MPReady)
        return Fail("create a console first");
    if (ms >= 0)
        MPInterface::Get().SetRecvTimeout(ms);
    return MPInterface::Get().GetRecvTimeout();
}

int mds_create(const char* bios9, const char* bios7, const char* firmware, const char* nickname, int debug)
{
    int id = -1;
    {
        std::lock_guard lock(SlotsLock);
        for (int i = 0; i < MaxInstances; i++)
            if (!Slots.count(i))
            {
                id = i;
                break;
            }
        if (id < 0)
            return Fail("too many consoles");
        if (!MPReady)
        {
            MPInterface::Set(MPInterface_Local);
            MPReady = true;
        }
    }

    Config config;
    config.BIOS9Path = bios9 ? bios9 : "";
    config.BIOS7Path = bios7 ? bios7 : "";
    config.FirmwarePath = firmware ? firmware : "";
    config.Nickname = nickname ? nickname : "";
    config.Debug = debug != 0;

    std::string error;
    auto inst = std::make_unique<Instance>(id, config, error);
    if (!error.empty())
        return Fail(error);

    Instance* created = inst.get();
    {
        std::lock_guard lock(SlotsLock);
        Slots[id].Inst = std::move(inst);
    }
    std::function<void(Instance&)> hook;
    {
        std::lock_guard lock(HookLock);
        hook = CreatedHook;
    }
    if (hook)
        hook(*created);
    return id;
}

int mds_destroy(int id)
{
    std::unique_ptr<Instance> inst;
    {
        std::lock_guard lock(SlotsLock);
        auto it = Slots.find(id);
        if (it == Slots.end())
            return Fail("no console " + std::to_string(id));
        inst = std::move(it->second.Inst);
        Slots.erase(it);
    }
    inst.reset();
    std::function<void(int)> hook;
    {
        std::lock_guard lock(HookLock);
        hook = DestroyedHook;
    }
    if (hook)
        hook(id);
    return 0;
}

int mds_load_rom(int id, const char* rom, const char* save)
{
    Slot* s = Idle(id, "loading a ROM needs an idle console");
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        return inst.LoadROM(rom, save ? save : "", error) ? 0 : -1;
    });
}

int mds_reset(int id)
{
    Slot* s = Idle(id, "reset needs an idle console");
    if (!s)
        return -1;
    return OnEmu(s, [](Instance& inst, std::string&) {
        inst.Reset();
        return 0;
    });
}

int mds_set_rtc(int id, int year, int month, int day, int hour, int minute, int second)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    if (year < 2000 || year > 2099)
        return Fail("RTC year must be 2000-2099");
    return OnEmu(s, [&](Instance& inst, std::string&) {
        inst.NDS->RTC.SetDateTime(year, month, day, hour, minute, second);
        return 0;
    });
}

int mds_get_rtc(int id, int* out)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        inst.NDS->RTC.GetDateTime(out[0], out[1], out[2], out[3], out[4], out[5]);
        return 0;
    });
}

int mds_run(const int* ids, int count, uint32_t frames, int timeout_ms, int* states)
{
    if (!CountOk(count, "count"))
        return -1;
    std::vector<Slot*> slots;
    for (int i = 0; i < count; i++)
    {
        Slot* s = Stopped(ids[i]);
        if (!s)
            return -1;
        slots.push_back(s);
    }
    for (Slot* s : slots)
        s->Inst->Start(frames);
    return mds_wait(ids, count, timeout_ms, states);
}

int mds_wait(const int* ids, int count, int timeout_ms, int* states)
{
    if (!CountOk(count, "count"))
        return -1;
    std::vector<Slot*> slots;
    for (int i = 0; i < count; i++)
    {
        Slot* s = Find(ids[i]);
        if (!s)
            return -1;
        slots.push_back(s);
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool interrupted = false;
    int result = 0;
    for (;;)
    {
        bool running = false, parked = false;
        Slot* waitOn = nullptr;
        for (Slot* s : slots)
        {
            RunState st = s->Inst->GetState();
            if (st == RunState::Running)
            {
                running = true;
                waitOn = s;
            }
            else if (st == RunState::Parked)
                parked = true;
        }
        if (parked && running && !interrupted)
        {
            for (Slot* s : slots)
                if (s->Inst->GetState() == RunState::Running)
                    s->Inst->Interrupt();
            interrupted = true;
        }
        if (!running)
            break;
        if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline)
        {
            result = 1;
            break;
        }
        waitOn->Inst->Wait(5);
    }
    if (states)
        for (int i = 0; i < count; i++)
            states[i] = StateCode(slots[i]->Inst->GetState());
    return result;
}

int mds_gdb(int id, int port9, int port7, int break_on_start)
{
    return mds_gdb_host(id, nullptr, port9, port7, break_on_start);
}

int mds_gdb_host(int id, const char* host, int port9, int port7, int break_on_start)
{
    Slot* s = StoppedDebug(id);
    if (!s)
        return -1;
#ifdef GDBSTUB_ENABLED
    std::string address = host && *host ? host : "127.0.0.1";
    uint32_t ip;
    if (!Net::ParseIPv4(address, ip))
        return Fail("bad host: " + address + " (an IPv4 address such as 127.0.0.1 or 0.0.0.0)");
    if (port9 <= 0 && port7 <= 0)
        return OnEmu(s, [&](Instance& inst, std::string&) {
            inst.NDS->SetGdbArgs(std::nullopt);
            return 0;
        });
    // GdbStub::Init warns (log level Warn) when the address is not loopback
    return OnEmu(s, [&](Instance& inst, std::string&) {
        GDBArgs args {(u16)port7, (u16)port9, break_on_start != 0, break_on_start != 0};
        args.BindAddress = address;
        inst.NDS->SetGdbArgs(args);
        return 0;
    });
#else
    return Fail("built without the GDB stub");
#endif
}

int mds_start(int id, uint32_t frames)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    s->Inst->Start(frames);
    return 0;
}

int mds_stop_after(int id, uint32_t frames)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    s->Inst->StopAfter(frames);
    return 0;
}

int mds_interrupt(int id)
{
    Slot* s = Debug(Find(id), id);
    if (!s)
        return -1;
    s->Inst->Interrupt();
    return 0;
}

int mds_state(int id)
{
    Slot* s = Find(id);
    return s ? StateCode(s->Inst->GetState()) : -1;
}

int mds_take_break(int id, mds_break* out)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    BreakEvent ev;
    if (!s->Inst->TakeBreak(ev))
        return 0;
    *out = {(int32_t)ev.Kind, ev.CPU ? 7 : 9, ev.Size, ev.ID, ev.PC, ev.Addr, ev.Value,
            ev.DMAChannel == 0xFF ? -1 : (int32_t)ev.DMAChannel};
    return 1;
}

uint64_t mds_frames(int id)
{
    Slot* s = Find(id);
    return s ? s->Inst->FramesRun : MDS_FRAMES_ERROR;
}

int mds_reset_count(int id)
{
    Slot* s = Find(id);
    return s ? (int)s->Inst->Resets.load() : -1;
}

int mds_set_render(int id, uint32_t flags)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    if (flags & ~(uint32_t)(MDS_RENDER_SKIP_2D | MDS_RENDER_SKIP_3D))
        return Fail("unknown render flags " + std::to_string(flags));
    s->Inst->WantedRenderSkip = flags;
    return 0;
}

int mds_set_threaded_3d(int id, int on)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    s->Inst->WantedThreaded3D = on != 0;
    // at once on a stopped console, at the next frame boundary on a running one (the command queue)
    return OnEmu(s, [](Instance& inst, std::string&) {
        inst.ApplyThreaded3D();
        return 0;
    });
}

int mds_render_stats(int id, uint64_t* out, int max)
{
    Slot* s = Find(id);
    if (!s || !CountOk(max, "max"))
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        SoftRenderer& rend = inst.Renderer();
        const SoftRenderer::RenderSkipStats& st = rend.GetRenderSkipStats();
        const uint64_t v[MDS_RENDER_STATS] = {
            inst.WantedRenderSkip.load(), rend.GetRenderSkip(),
            inst.WantedThreaded3D.load() ? 1u : 0u, rend.IsThreaded3D() ? 1u : 0u,
            st.Lines2DSkipped, st.Frames3DSkipped, st.CaptureLines, st.Frames3DForCapture, st.Late3DFrames,
        };
        for (int i = 0; i < MDS_RENDER_STATS && i < max; i++)
            out[i] = v[i];
        return MDS_RENDER_STATS;
    });
}

int mds_screen(int id, uint32_t* out)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        void* top = nullptr;
        void* bottom = nullptr;
        if (!inst.NDS->GPU.GetFramebuffers(&top, &bottom) || !top || !bottom)
        {
            error = "no framebuffer";
            return -1;
        }
        memcpy(out, top, 256 * 192 * 4);
        memcpy(out + 256 * 192, bottom, 256 * 192 * 4);
        return 0;
    });
}

// The setters go through the same queue as every other source: they amend the newest queued
// setter payload if there is one, so keys and touch set before a frame land in that frame.
static int Modify(Slot* s, const std::function<void(InputPayload&)>& fn)
{
    std::string error;
    if (s->Inst->Input.Modify(fn, error) < 0)
        return Fail(error);
    return 0;
}

int mds_set_keys(int id, uint32_t pressed)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return Modify(s, [&](InputPayload& p) { p.Buttons = pressed & 0xFFF; });
}

int mds_touch(int id, int x, int y)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return Modify(s, [&](InputPayload& p) {
        if (x < 0 || y < 0)
            p.Touch = {{0, 0, 0, 0}};
        else
            p.Touch = {{0, (u16)std::min(x, 255), (u16)std::min(y, 191), 1}};
    });
}

int mds_set_lid(int id, int closed)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return Modify(s, [&](InputPayload& p) { p.Lid = closed != 0; });
}

// False with *error set ("bad_payload: ...") if the arguments cannot make a payload.
static bool BuildPayload(const mds_input* in, const mds_touch_point* points, const int16_t* mic, InputPayload& p,
                         std::string& error)
{
    if (!in)
    {
        error = "bad_payload: no input";
        return false;
    }
    if ((in->touch_count && !points) || (in->mic_count && !mic))
    {
        error = "bad_payload: missing touch or microphone data";
        return false;
    }
    p.Source = (in->flags & 1) ? InputSource::Client : InputSource::Local;
    p.Seq = in->seq;
    p.Buttons = in->buttons;
    p.Lid = in->lid != 0;
    for (int i = 0; i < in->touch_count; i++)
        p.Touch.push_back({points[i].offset, points[i].x, points[i].y, points[i].down});
    p.MicKind = in->mic_kind;
    if (in->mic_count)
        p.Mic.assign(mic, mic + in->mic_count);
    p.MicLevel = in->mic_level;
    return true;
}

int mds_input_submit(int id, const mds_input* in, const mds_touch_point* points, const int16_t* mic)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    InputPayload p;
    std::string error;
    if (!BuildPayload(in, points, mic, p, error))
        return Fail(error);
    int depth = s->Inst->Input.Submit(std::move(p), error);
    if (depth < 0)
        return Fail(error);
    return depth;
}

int mds_debug_recv_pending(int id, int pending)
{
    if (!Find(id))
        return -1;
    Barriers().SetRecvPending(id, pending != 0);
    return 0;
}

int mds_input_submit_multi(const int* ids, int count, const mds_input* inputs, const mds_touch_point* const* points,
                           const int16_t* const* mic, int* depths)
{
    if (!ids || count < 1 || !inputs)
        return Fail("bad_payload: no entries");
    std::vector<GroupEntry> entries;
    for (int i = 0; i < count; i++)
    {
        Slot* s = Find(ids[i]);
        if (!s)
            return -1;
        GroupEntry e;
        e.Queue = &s->Inst->Input;
        e.Console = ids[i];
        std::string error;
        if (!BuildPayload(&inputs[i], points ? points[i] : nullptr, mic ? mic[i] : nullptr, e.Payload, error))
            return Fail(error);
        entries.push_back(std::move(e));
    }
    std::vector<int> out;
    std::string error;
    if (!SubmitGroup(entries, out, error))
        return Fail(error);
    if (depths)
        std::copy(out.begin(), out.end(), depths);
    return 0;
}

int mds_input_mode(int id, int mode, int stall_frames)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    if (mode < -1 || mode > 2)
        return Fail("unknown input mode");
    if (stall_frames >= 0)
        s->Inst->Input.SetStallTimeout(stall_frames);
    if (mode >= 0)
        s->Inst->Input.SetMode((EmptyMode)mode);
    return 0;
}

int mds_input_depth(int id)
{
    Slot* s = Find(id);
    return s ? (int)s->Inst->Input.Depth() : -1;
}

int mds_input_waiting(int id)
{
    Slot* s = Find(id);
    return s ? (s->Inst->WaitingForInput() ? 1 : 0) : -1;
}

int mds_input_clear(int id)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    s->Inst->Input.Clear();
    return 0;
}

int mds_input_log(int id, uint64_t from_frame, mds_input_log_entry* out, int max)
{
    Slot* s = Find(id);
    if (!s || !CountOk(max, "max"))
        return -1;
    int n = 0;
    for (const InputLogEntry& e : s->Inst->Input.ReadLog(from_frame, max))
        out[n++] = {e.Frame, e.Seq, e.Buttons, e.Lid, e.TouchCount, e.MicKind, 0, e.MicLevel, 0, 0};
    return n;
}

int mds_read(int id, int cpu, uint32_t addr, uint32_t len, uint8_t* out)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        Api::ReadGuest(inst, cpu, addr, len, out);
        return 0;
    });
}

// Applied by the emulator thread at the next frame boundary (or at once when idle or parked).
int mds_write(int id, int cpu, uint32_t addr, uint32_t len, const uint8_t* data)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        return inst.NDS->Debugger.WriteMemory(cpu == 7 || cpu == 1, addr, data, len, error) ? 0 : -1;
    });
}

int mds_get_regs(int id, int cpu, uint32_t* out)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        int n = cpu == 7 || cpu == 1;
        ARM& arm = CPU(inst, n);
        for (int i = 0; i < 15; i++)
            out[i] = arm.R[i];
        // The instruction the CPU runs next: both CPUs are between two instructions when idle and at every
        // park (an instruction break parks before its instruction, a watchpoint at the next one, a DMA break
        // between two DMA units, a request at a frame boundary or an instruction), except at a fault, which
        // parks inside the faulting instruction: there it is that instruction until the pc is changed.
        out[15] = NextInstruction(arm);
        if (inst.GetState() == RunState::Parked)
        {
            const BreakEvent& ev = inst.ParkEvent();
            if (ev.Kind == BreakKind::Fault && ev.CPU == n && arm.R[15] == inst.ParkR15(n))
                out[15] = ev.PC;
        }
        out[16] = arm.CPSR;
        out[17] = SPSR(arm);
        out[18] = arm.R[15];
        return 0;
    });
}

int mds_set_reg(int id, int cpu, int reg, uint32_t value)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        ARM& arm = CPU(inst, cpu == 7 || cpu == 1);
        if (reg < 0 || reg > 15)
        {
            error = "register out of range";
            return -1;
        }
        if (reg == 15)
            arm.JumpTo(value | ((arm.CPSR & 0x20) ? 1 : 0));
        else
            arm.R[reg] = value;
        return 0;
    });
}

int mds_bp_add(int id, uint32_t cpumask, uint32_t addr)
{
    Slot* s = StoppedDebug(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        return inst.NDS->Debugger.AddBreakpoint(cpumask & 3, addr);
    });
}

int mds_wp_add(int id, uint32_t cpumask, uint32_t addr, uint32_t size, uint32_t flags)
{
    Slot* s = StoppedDebug(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        int r = inst.NDS->Debugger.AddWatchpoint(cpumask & 3, addr, size, flags & 3);
        if (r < 0)
            error = "bad watchpoint";
        return r;
    });
}

int mds_point_remove(int id, int point)
{
    Slot* s = StoppedDebug(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        // A Lua hook removed from here would leave the script with a dead id (and its callback registered).
        if (inst.NDS->Debugger.IsHook(point))
        {
            error = "point " + std::to_string(point) + " is a Lua hook: the script removes it "
                    "(event.unregisterbyid / unregisterbyname)";
            return -1;
        }
        if (inst.NDS->Debugger.Remove(point))
            return 0;
        error = "no such point";
        return -1;
    });
}

int mds_point_enable(int id, int point, int enabled)
{
    Slot* s = StoppedDebug(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        if (inst.NDS->Debugger.SetEnabled(point, enabled != 0))
            return 0;
        error = "no such point";
        return -1;
    });
}

int mds_point_list(int id, mds_point* out, int max)
{
    Slot* s = StoppedDebug(id);
    if (!s || !CountOk(max, "max"))
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        auto& dbg = inst.NDS->Debugger;
        int n = 0;
        for (auto& b : dbg.GetBreakpoints())
            if (n < max)
                out[n++] = {b.ID, 0, b.CPUMask, b.Callback ? MDS_POINT_LUA : 0u, b.Enabled, b.Addr, b.Addr, b.Hits};
        for (auto& w : dbg.GetWatchpoints())
            if (n < max)
                out[n++] = {w.ID, 1, w.CPUMask, w.Flags | (w.Callback ? MDS_POINT_LUA : 0u), w.Enabled, w.Start, w.End,
                            w.Hits};
        return n;
    });
}

int mds_point_count(int id)
{
    Slot* s = StoppedDebug(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        return int(inst.NDS->Debugger.GetBreakpoints().size() + inst.NDS->Debugger.GetWatchpoints().size());
    });
}

int mds_step(int id, int cpu, uint32_t count)
{
    Slot* s = StoppedDebug(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        inst.NDS->Debugger.Step(cpu == 7 || cpu == 1, count);
        return 0;
    });
}

int mds_trace(int id, uint32_t cpumask, uint32_t start, uint32_t end, uint32_t capacity)
{
    Slot* s = StoppedDebug(id);
    if (!s)
        return -1;
    s->Trace.clear();
    return OnEmu(s, [&](Instance& inst, std::string&) {
        inst.NDS->Debugger.SetTrace(cpumask & 3, start, end, capacity);
        return 0;
    });
}

int mds_trace_take(int id, mds_trace_entry* out, int max)
{
    Slot* s = StoppedDebug(id);
    if (!s || !CountOk(max, "max"))
        return -1;
    std::vector<TraceEntry> buf(max);
    int n = Take(s->Trace, [s] {
        std::vector<TraceEntry> drained;
        s->Inst->Call([&](Instance& inst) { drained = inst.NDS->Debugger.DrainTrace(); });
        return drained;
    }, buf.data(), max);
    for (int i = 0; i < n; i++)
        out[i] = {buf[i].Timestamp, buf[i].PC, buf[i].Instr, buf[i].CPSR, buf[i].CPU ? 7u : 9u};
    return n;
}

int mds_calls(int id, int enabled)
{
    Slot* s = StoppedDebug(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        inst.NDS->Debugger.SetCallTracking(enabled != 0);
        return 0;
    });
}

int mds_callstack(int id, int cpu, mds_frame* out, int max)
{
    Slot* s = StoppedDebug(id);
    if (!s || !CountOk(max, "max"))
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        auto stack = inst.NDS->Debugger.GetCallStack(cpu == 7 || cpu == 1);
        int n = std::min<int>(max, stack.size());
        for (int i = 0; i < n; i++)
        {
            const CallFrame& f = stack[stack.size() - 1 - i];
            out[i] = {f.Site, f.Entry, f.Return, f.SP, f.Mode, f.Exception};
        }
        return n;
    });
}

int mds_threadstacks(int id, int cpu, uint32_t* sps, uint32_t* depths, int max_stacks, mds_frame* out, int max_frames)
{
    Slot* s = StoppedDebug(id);
    if (!s || !CountOk(max_stacks, "max_stacks") || !CountOk(max_frames, "max_frames"))
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        auto threads = inst.NDS->Debugger.GetThreadStacks(cpu == 7 || cpu == 1);
        int count = std::min<int>(max_stacks, threads.size());
        int used = 0;
        for (int t = 0; t < count; t++)
        {
            const auto& stack = threads[t].second;
            int n = std::min<int>(max_frames - used, stack.size());
            for (int i = 0; i < n; i++)
            {
                const CallFrame& f = stack[stack.size() - 1 - i];
                out[used + i] = {f.Site, f.Entry, f.Return, f.SP, f.Mode, f.Exception};
            }
            sps[t] = threads[t].first;
            depths[t] = n;
            used += n;
        }
        return count;
    });
}

int mds_callcounts_take(int id, int cpu, uint32_t* entries, uint64_t* counts, int max)
{
    Slot* s = StoppedDebug(id);
    if (!s || !CountOk(max, "max"))
        return -1;
    int n = cpu == 7 || cpu == 1;
    std::vector<std::pair<u32, u64>> buf(max);
    int got = Take(s->Calls[n], [s, n] {
        std::vector<std::pair<u32, u64>> drained;
        s->Inst->Call([&](Instance& inst) {
            auto map = inst.NDS->Debugger.DrainCallCounts(n);
            drained.assign(map.begin(), map.end());
        });
        return drained;
    }, buf.data(), max);
    for (int i = 0; i < got; i++)
    {
        entries[i] = buf[i].first;
        counts[i] = buf[i].second;
    }
    return got;
}

int mds_coverage(int id, int enabled)
{
    Slot* s = StoppedDebug(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        inst.NDS->Debugger.SetCoverage(enabled != 0);
        return 0;
    });
}

int mds_coverage_take(int id, int cpu, uint32_t* out, int max)
{
    Slot* s = StoppedDebug(id);
    if (!s || !CountOk(max, "max"))
        return -1;
    int n = cpu == 7 || cpu == 1;
    return Take(s->Coverage[n], [s, n] {
        std::vector<u32> drained;
        s->Inst->Call([&](Instance& inst) { drained = inst.NDS->Debugger.DrainCoverage(n); });
        return drained;
    }, out, max);
}

int mds_lastwriter(int id, int enabled)
{
    Slot* s = StoppedDebug(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        inst.NDS->Debugger.SetLastWriter(enabled != 0);
        return 0;
    });
}

uint32_t mds_lastwriter_get(int id, uint32_t addr)
{
    uint32_t raw = 0;
    return mds_lastwriter_query(id, addr, &raw, nullptr) < 0 ? 0 : raw;
}

int mds_lastwriter_query(int id, uint32_t addr, uint32_t* raw, int* dma)
{
    Slot* s = StoppedDebug(id);
    if (!s)
        return -1;
    uint32_t writer = 0;
    bool tracking = false;
    if (!s->Inst->Call([&](Instance& inst) {
            tracking = inst.NDS->Debugger.IsTrackingWriters();
            writer = inst.NDS->Debugger.GetLastWriter(addr);
        }))
        return Fail("console is shutting down");
    if (!tracking)
        return Fail("last-writer tracking is off (mds_lastwriter(id, 1))");
    if (raw)
        *raw = writer;
    if (dma)
        *dma = writer >= DMAWriterTag ? int((writer >> 1) & 3) : -1;
    return writer ? 1 : 0;
}

int mds_overlays(int id, int* ids, uint32_t* bases, uint32_t* sizes, int max)
{
    Slot* s = Stopped(id);
    if (!s || !CountOk(max, "max"))
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        int n = 0;
        for (const RomOverlay& ov : ReadRomOverlays(*inst.NDS))
        {
            if (!OverlayLoaded(*inst.NDS, ov.Base, ov.Size, ov.Image))
                continue;
            if (n < max)
            {
                if (ids) ids[n] = ov.Id;
                if (bases) bases[n] = ov.Base;
                if (sizes) sizes[n] = ov.Size;
            }
            n++;
        }
        return n;
    });
}

int mds_savestate(int id, const char* path)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        return inst.SaveState(path, error) ? 0 : -1;
    });
}

int mds_loadstate(int id, const char* path)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        return inst.LoadState(path, error) ? 0 : -1;
    });
}

// Action Replay codes. The list lives on the console (Instance::Cheats) and feeds the core's AREngine; every
// change runs on the emulator thread, so it never races the engine, which runs inside the frame.
int mds_cheat_add(int id, const char* name, const char* code, int enabled)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    if (!code)
        return Fail("cheat: no code text");
    std::string n = name ? name : "";
    if (n.size() > MaxCheatName)
        return Fail("cheat: the name is longer than " + std::to_string(MaxCheatName) + " bytes");
    std::vector<uint32_t> words;
    std::string error;
    if (!ParseCheatCode(code, words, error))
        return Fail("cheat: " + error);
    return OnEmu(s, [&](Instance& inst, std::string&) {
        return inst.AddCheat(n, std::move(words), enabled != 0);
    });
}

int mds_cheat_remove(int id, int cheat)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        if (inst.RemoveCheat(cheat))
            return 0;
        error = "no cheat " + std::to_string(cheat);
        return -1;
    });
}

int mds_cheat_enable(int id, int cheat, int enabled)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        if (inst.EnableCheat(cheat, enabled != 0))
            return 0;
        error = "no cheat " + std::to_string(cheat);
        return -1;
    });
}

int mds_cheat_clear(int id)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return OnEmu(s, [](Instance& inst, std::string&) { return inst.ClearCheats(); });
}

int mds_cheat_list(int id, mds_cheat* out, int max)
{
    Slot* s = Find(id);
    if (!s || !CountOk(max, "max"))
        return -1;
    if (!out && max > 0)
        return Fail("no buffer");
    return OnEmu(s, [&](Instance& inst, std::string&) {
        int n = 0;
        for (const CheatEntry& c : inst.GetCheats())
        {
            if (n >= max)
                break;
            mds_cheat& e = out[n++];
            memset(&e, 0, sizeof(e));
            e.cheat = c.ID;
            e.enabled = c.Enabled;
            e.words = (int32_t)c.Code.size();
            strncpy(e.name, c.Name.c_str(), sizeof(e.name) - 1);
        }
        return n;
    });
}

int mds_cheat_get(int id, int cheat, char* name, int name_max, uint32_t* words, int max_words)
{
    Slot* s = Find(id);
    if (!s || !CountOk(name_max, "name_max") || !CountOk(max_words, "max_words"))
        return -1;
    if ((!name && name_max > 0) || (!words && max_words > 0))
        return Fail("no buffer");
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        for (const CheatEntry& c : inst.GetCheats())
        {
            if (c.ID != cheat)
                continue;
            if (name_max > 0)
            {
                size_t n = std::min(c.Name.size(), (size_t)name_max - 1);
                memcpy(name, c.Name.data(), n);
                name[n] = 0;
            }
            int n = std::min<int>(max_words, (int)c.Code.size());
            std::copy(c.Code.begin(), c.Code.begin() + n, words);
            return (int)c.Code.size();
        }
        error = "no cheat " + std::to_string(cheat);
        return -1;
    });
}

// The Lua calls run on the emulator thread like every other command, so they are serialised with the other
// threads' calls (the socket server's included) and the script touches the console only from its own thread.
int mds_lua_run(int id, const char* source, int is_file)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    if (!source)
        return Fail("no Lua source");
    std::string src = source;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        return inst.Lua->Run(src, is_file != 0, error) ? 0 : -1;
    });
}

int mds_lua_exec(int id, const char* code)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    if (!code)
        return Fail("no Lua code");
    std::string chunk = code;
    return OnEmu(s, [&](Instance& inst, std::string& error) {
        return inst.Lua->Exec(chunk, error) ? 0 : -1;
    });
}

int mds_lua_stop(int id)
{
    Slot* s = Stopped(id);
    if (!s)
        return -1;
    return OnEmu(s, [](Instance& inst, std::string&) {
        inst.Lua->Stop();
        return 0;
    });
}

int mds_lua_output(int id, char* out, int max)
{
    Slot* s = Find(id);
    if (!s || !CountOk(max, "max"))
        return -1;
    s->LuaOutput += s->Inst->Lua->TakeOutput();
    int n = std::min<int>(max, s->LuaOutput.size());
    memcpy(out, s->LuaOutput.data(), n);
    s->LuaOutput.erase(0, n);
    return n;
}

int mds_packets(int id, int enabled, int capacity)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    s->Packets.clear();
    s->Inst->SetPacketLog(enabled != 0, std::max(capacity, 1));
    return 0;
}

int mds_packets_take(int id, mds_packet* headers, uint8_t* data, int max_packets, int max_data)
{
    Slot* s = Find(id);
    if (!s || !CountOk(max_packets, "max_packets") || !CountOk(max_data, "max_data"))
        return -1;
    if (s->Packets.empty())
        s->Packets = s->Inst->DrainPackets();
    int n = 0, used = 0;
    while (n < max_packets && n < (int)s->Packets.size())
    {
        auto& p = s->Packets[n];
        int size = (int)p.Data.size();
        if (used + size > max_data)
        {
            if (n > 0)
                break;
            size = max_data;        // the first one does not fit: cut it short rather than block every call
        }
        headers[n] = {p.Timestamp, p.Sent, p.Type, (uint32_t)size, (uint32_t)p.Data.size()};
        memcpy(data + used, p.Data.data(), size);
        used += size;
        n++;
    }
    s->Packets.erase(s->Packets.begin(), s->Packets.begin() + n);
    return n;
}

int mds_debug_packet(int id, int sent, int type, const uint8_t* data, int len)
{
    Slot* s = Find(id);
    if (!s || !CountOk(len, "len"))
        return -1;
    if (!data && len > 0)
        return Fail("no data");
    s->Inst->LogPacket(sent != 0, (u8)type, data, len, 0);
    return 0;
}

int mds_serve(int port, int flags, int* bound)
{
    return mds_serve_host(nullptr, port, flags, bound);
}

int mds_serve_host(const char* host, int port, int flags, int* bound)
{
    if (port < 0 || port > 65535)
        return Fail("bad port");
    std::string address = host && *host ? host : "127.0.0.1";
    uint32_t ip;
    if (!Net::ParseIPv4(address, ip))
        return Fail("bad host: " + address + " (an IPv4 address such as 127.0.0.1 or 0.0.0.0)");
    std::string error;
    int actual = 0;
    if (!ServerStart(ip, port, flags & 0xFFFF, actual, error))
        return Fail(error);
    if ((ip >> 24) != 127)
        fprintf(stderr, "melonDS: warning: the socket server listens on %s:%d, not loopback: anyone who can "
                "reach this machine can control the emulator\n", address.c_str(), actual);
    if (bound)
        *bound = actual;
    return 0;
}

void mds_serve_stop(void)
{
    ServerStop();
}

int mds_serve_wait(int timeout_ms)
{
    return ServerWait(timeout_ms);
}

void mds_serve_name(const char* name)
{
    ServerSetName(name ? name : "");
}

int mds_set_instance_base(int base)
{
    {
        std::lock_guard lock(SlotsLock);
        if (!Slots.empty())
            return Fail("the instance base can only change while no console exists");
    }
    if (!SetInstanceBase(base))
        return Fail("instance base must not be negative");
    return 0;
}

int mds_instance_base(void)
{
    return InstanceBase();
}

int mds_mac(int id, uint8_t* out)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    return OnEmu(s, [&](Instance& inst, std::string&) {
        const auto& mac = inst.NDS->GetFirmware().GetHeader().MacAddr;
        memcpy(out, mac.data(), 6);
        return 0;
    });
}

int mds_save_path(int id, char* out, int max)
{
    Slot* s = Find(id);
    if (!s)
        return -1;
    if (!out && max > 0)
        return Fail("no buffer");
    const std::string& path = s->Inst->GetSavePath();
    if (max <= 0 || (int)path.size() >= max)
        return Fail("buffer too small");
    memcpy(out, path.c_str(), path.size() + 1);
    return (int)path.size();
}

}

namespace Headless::Api
{

Instance* FindInstance(int id)
{
    std::lock_guard lock(SlotsLock);
    auto it = Slots.find(id);
    return it == Slots.end() ? nullptr : it->second.Inst.get();
}

std::vector<int> ConsoleIds()
{
    std::lock_guard lock(SlotsLock);
    std::vector<int> ids;
    for (auto& [id, slot] : Slots)
        if (slot.Inst)
            ids.push_back(id);
    return ids;
}

bool MoreStaged(int id, Staged what, int cpu)
{
    std::lock_guard lock(SlotsLock);
    auto it = Slots.find(id);
    if (it == Slots.end())
        return false;
    Slot& s = it->second;
    int n = cpu == 7 || cpu == 1;
    switch (what)
    {
    case Staged::Trace: return !s.Trace.empty();
    case Staged::CallCounts: return !s.Calls[n].empty();
    case Staged::Coverage: return !s.Coverage[n].empty();
    case Staged::Packets: return !s.Packets.empty();
    }
    return false;
}

void SetConsoleHooks(std::function<void(Instance&)> created, std::function<void(int)> destroyed)
{
    std::lock_guard lock(HookLock);
    CreatedHook = std::move(created);
    DestroyedHook = std::move(destroyed);
}

void SetError(const std::string& error)
{
    LastError = error;
}

void ReadGuest(Instance& inst, int cpu, uint32_t addr, uint32_t len, uint8_t* out)
{
    melonDS::NDS& nds = *inst.NDS;
    int n = cpu == 7 || cpu == 1;
    ARMv5& arm9 = *nds.ARM9;
    for (u32 i = 0; i < len; i++)
    {
        u32 a = addr + i;
        bool tcm = !n && (a < arm9.ITCMSize || (a & arm9.DTCMMask) == arm9.DTCMBase);
        if ((a >> 24) == 0x02 && !tcm)
            out[i] = nds.MainRAM[a & nds.MainRAMMask];
        else
            out[i] = nds.Debugger.Peek(n, a, 1);
    }
}

}
