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

#ifndef DEBUGGER_H
#define DEBUGGER_H

#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../types.h"
#include "Probes.h"

namespace melonDS
{
class ARM;
class NDS;

enum class BreakKind : u8
{
    Exec,
    Read,
    Write,
    Step,
    Request,
    Fault,
    // Not a park: a GDB client reset the console or a CPU (ResetHandler). Frontends that queue break events
    // report it; the console keeps running.
    Reset,
};

enum class StepMode : u8
{
    None,
    Branch,
    TakenBranch,
};

enum class ExecResult : u8
{
    Refused,
    Continued,
    Branched,
};

enum WatchFlags : u8
{
    Watch_Read = 1,
    Watch_Write = 2,
};

// The pc of a break or last-writer record made by DMA, not by an instruction: 0xFFFFFFF0 | channel << 1 | arm7.
constexpr u32 DMAWriterTag = 0xFFFFFFF0;

struct BreakEvent
{
    BreakKind Kind;
    u8 CPU;
    u8 Size;
    int ID;
    u32 PC;
    u32 Addr;
    u32 Value;
    u8 DMAChannel = 0xFF;
};

using BreakCallback = std::function<bool(const BreakEvent&)>;

struct WifiFrame
{
    bool TX;
    u8 Rate;
    u8 Channel;
    u64 Timestamp;
    std::vector<u8> Data;
};

using WifiCallback = std::function<bool(WifiFrame&)>;

struct Breakpoint
{
    int ID;
    u8 CPUMask;
    bool Enabled;
    u32 Addr;
    u64 Hits;
    BreakCallback Callback;
};

struct Watchpoint
{
    int ID;
    u8 CPUMask;
    u8 Flags;
    bool Enabled;
    u32 Start;
    u32 End;
    u64 Hits;
    BreakCallback Callback;
};

struct TraceEntry
{
    u64 Timestamp;
    u32 PC;
    u32 Instr;
    u32 CPSR;
    u8 CPU;
};

struct CallFrame
{
    u32 Site;
    u32 Entry;
    u32 Return;
    u32 SP;
    u8 Mode;
    bool Exception;
    bool HasArgs = false;
    u32 Args[4] = {};
};

class Debugger
{
public:
    explicit Debugger(melonDS::NDS& nds) noexcept;

    void Reset();

    int AddBreakpoint(u8 cpuMask, u32 addr, BreakCallback callback = nullptr);
    int AddWatchpoint(u8 cpuMask, u32 start, u32 size, u8 flags, BreakCallback callback = nullptr);
    int AddWifiHook(WifiCallback callback);
    bool WifiHooked() const { return !WifiHooks.empty(); }
    bool RunWifiHooks(WifiFrame& frame);
    void InjectWifi(std::vector<u8> frame, u8 rate);
    bool TakeWifiInjection(WifiFrame& frame);
    bool Remove(int id);
    // True for an id whose owner is code, not a client: a point with a callback (a Lua memory hook, a Qt debugger
    // temporary) or a wifi hook.
    bool IsHook(int id) const;
    bool SetEnabled(int id, bool enabled);
    void ClearAll();
    const std::vector<Breakpoint>& GetBreakpoints() const { return Breakpoints; }
    const std::vector<Watchpoint>& GetWatchpoints() const { return Watchpoints; }

    void SetBreakHandler(std::function<void(const BreakEvent&)> handler);
    void SetServiceHandler(std::function<void()> handler);
    void RequestService();
    void RequestBreak(u8 cpuMask);
    // Drop the break requests of both CPUs, and with steps also the armed step countdowns.
    void CancelBreakRequests(bool steps = true);
    // Park the console after the CPU has executed count more instructions (0 disarms). Counted from the
    // instruction the CPU is parked at when called during a break of that CPU's instruction hook, else from
    // its next instruction, so count = n always executes n instructions.
    void Step(u8 cpu, u32 count);
    // Smart stepping (the Qt debugger's "step to branch"/"run to taken branch"): park before the next
    // instruction that may write PC at the current call depth, or after the next taken branch. Separate
    // from the Step countdown; a break of any kind cancels it.
    void StepTo(u8 cpu, StepMode mode);
    u32 CallReturn(u8 cpu, u32 pc, bool thumb, u32 lr) const;

    // After a savestate load: drop what described the execution before it (call stacks, a pending
    // watchpoint hit, the last-writer records). Points, the trace, call counts, coverage and armed
    // steps/break requests are kept.
    void OnStateLoaded();

    void SetTrace(u8 cpuMask, u32 start, u32 end, u32 capacity);
    std::vector<TraceEntry> DrainTrace();

    void SetCallTracking(bool enabled);
    std::vector<CallFrame> GetCallStack(u8 cpu) const;
    std::vector<std::pair<u32, std::vector<CallFrame>>> GetThreadStacks(u8 cpu) const;
    std::unordered_map<u32, u64> DrainCallCounts(u8 cpu);

    void SetCoverage(bool enabled);
    std::vector<u32> DrainCoverage(u8 cpu);

    void SetLastWriter(bool enabled);
    u32 GetLastWriter(u32 addr) const;
    bool IsTrackingWriters() const { return !LastWriter.empty(); }

    u32 Peek(u8 cpu, u32 addr, u8 size) const;
    bool Poke(u8 cpu, u32 addr, u8 size, u32 value);
    // Write len bytes as the CPU's bus sees them, for debugger clients (the C API, Lua). Byte writes where the
    // bus takes them; on the ARM9 the palette, VRAM and OAM ignore 8-bit writes, so they get aligned halfwords
    // (a byte at an odd edge is merged with the one beside it). Every write is read back. False with error
    // naming the region for I/O (never written: side effects), the BIOSes and the GBA ROM area (read-only), and
    // for a write that did not take (unmapped VRAM, palette powered off, no GBA cart for its SRAM, ...); the
    // bytes before the failing one are written.
    bool WriteMemory(u8 cpu, u32 addr, const u8* data, u32 len, std::string& error);
    // The name of the memory region an address is in, as the CPU sees it ("main RAM", "VRAM", ...).
    const char* RegionName(u8 cpu, u32 addr) const;
    void SyncPipelines(u32 addr, u32 size);
    int HookedCPU() const;
    ExecResult Execute(u8 cpu, const std::vector<u32>& ops);

    bool IsBreaking() const { return InBreak; }

    std::array<bool, 2> Armed {};
    std::array<bool, 2> DataArmed {};
    bool CallsArmed = false;

    // redirected: a repeat call for the same instruction slot because a hook or a break changed PC (see
    // ARM::GdbCheckC); the instruction of the first call never ran, so the step countdown does not advance.
    void OnInstr(ARM& cpu, u32 pc, bool redirected = false);
    void OnData(ARM& cpu, u32 addr, u8 size, u32 value, bool write);
    // A DMA unit is about to be written. The memory log, the last writer and watch callbacks see memory before
    // the write, as for a CPU store; a break the unit asks for is raised by OnDMAWritten, after the write, so
    // the break sees the new memory and parks between this unit and the next.
    void OnDMA(u8 cpu, u8 channel, u32 src, u32 addr, u8 size, u32 value);
    void OnDMAWritten(u8 cpu)
    {
        if (DMABreakValid[cpu])
            RaiseDMABreak(cpu);
    }
    // A DMA channel starts a new transfer or finishes one. One transfer breaks at most once: the first watched
    // unit that asks for a break parks, later units of the same transfer only run callbacks and count hits.
    void OnDMATransfer(u8 cpu, u8 channel) { DMABroke[cpu & 1][channel & 3] = false; }
    void OnCall(ARM& cpu);
    void OnException(ARM& cpu, u32 ret);
    void OnFault(ARM& cpu, u32 pc, u32 addr);

    std::array<u32, 2> CurPC {};
    std::function<void(u32 cpu, const char* text, bool newline)> PrintHandler;

    // Probes, the memory access log, breakpoint actions and overlay tags (Probes.h).
    DebugProbes Probes;
    // The call tracker's depth for the CPU (its frames and those of the contexts interrupted), -1 with tracking off.
    int CallDepth(u8 cpu) const;
    // A write made from inside a hook (a breakpoint action): refetches the pipeline if it covers the code there.
    void PokeFromHook(u8 cpu, u32 addr, u8 size, u32 value);
    // Called on the emulator thread after the GDB stub of CPU cpu has reset the whole console (vRun, R, r;
    // console = true) or only that CPU ("monitor reset"; console = false).
    std::function<void(u8 cpu, bool console)> ResetHandler;

private:
    friend class DebugProbes;
    melonDS::NDS& NDS;
    int NextID = 1;
    std::vector<Breakpoint> Breakpoints;
    std::vector<Watchpoint> Watchpoints;
    std::vector<std::pair<int, WifiCallback>> WifiHooks;
    std::deque<WifiFrame> WifiInjected;
    std::array<std::vector<u8>, 2> ExecPages;
    std::array<std::vector<u8>, 2> WatchPages;
    std::function<void(const BreakEvent&)> Handler;
    std::function<void()> ServiceHandler;
    std::atomic<bool> ServiceRequested {false};
    std::array<std::atomic<bool>, 2> BreakRequested {};
    std::array<u32, 2> StepRemaining {};
    std::array<bool, 2> PendingValid {};
    std::array<BreakEvent, 2> Pending {};
    std::array<bool, 2> DMABreakValid {};   // a DMA watch hit to raise once its unit is written (OnDMAWritten)
    std::array<BreakEvent, 2> DMABreak {};
    std::array<std::array<bool, 4>, 2> DMABroke {};  // the channel's current transfer has broken already
    void RaiseDMABreak(u8 n);
    bool InBreak = false;
    int InstrBreakCPU = -1;     // the CPU whose OnInstr is parked in Raise or runs breakpoint callbacks, else -1

    u8 TraceMask = 0;
    u32 TraceStart = 0, TraceEnd = 0;
    std::vector<TraceEntry> Trace;
    size_t TraceHead = 0;
    bool TraceWrapped = false;

    bool CallTracking = false;
    std::array<std::vector<CallFrame>, 2> CallStack;
    std::array<std::map<u32, std::vector<CallFrame>>, 2> ParkedStacks;
    std::array<std::array<u32, 16>, 2> LastSP {};
    std::array<u8, 2> LastMode {};
    std::array<u32, 2> PrevInstr {};
    std::array<u32, 2> JumpTarget {};
    std::array<u32, 2> Prev2Instr {};
    std::array<StepMode, 2> StepModes {};
    std::array<size_t, 2> StepDepth {};
    std::array<CallFrame, 2> StepFrame {};
    std::array<size_t, 2> StepExceptions {};
    std::array<bool, 2> SeedPending {};
    std::array<std::unordered_set<u32>, 2> KnownEntries;
    std::array<bool, 2> PrevThumb {};

    struct Interrupted
    {
        u32 Return;
        u32 SP;
        std::vector<CallFrame> Stack;
    };
    std::array<std::vector<Interrupted>, 2> Exceptions;
    std::array<std::unordered_map<u32, u64>, 2> CallCounts;

    bool Coverage = false;
    std::array<std::unordered_map<u32, std::array<u64, 32>>, 2> CoveragePages;
    std::array<u32, 2> CoverageLastPage {};
    std::array<std::array<u64, 32>*, 2> CoverageLastBits {};

    std::vector<u32> LastWriter;

    enum class HookContext : u8
    {
        None,
        Service,
        Hook,
    };
    struct ContextGuard;
    HookContext Context = HookContext::None;
    u8 ContextCPU = 0;
    std::thread::id ContextThread;
    std::array<int, 2> MidInstr {};

    struct ReturnSkip
    {
        u32 PC;
        u32 SP;
        u8 Mode;
        bool Left;
        std::array<u32, 4> Args;
        u32 R12;
        u32 LR;
        u32 Flags;
    };
    std::array<std::vector<ReturnSkip>, 2> ReturnSkips;

    bool OnContextThread() const;
    u32 Canonical(u8 cpu, u32 addr) const;
    bool PipelineCurrent(ARM& cpu) const;
    void Redirected(ARM& cpu, bool thumb, u32 op, u32 lr, u32 sp, u8 callerMode, const ReturnSkip& entry);
    bool ReturnedToHook(ARM& cpu, u32 pc);
    void Rearm();
    void RebuildPages();
    void Raise(const BreakEvent& ev);
    void UnwindCalls(ARM& cpu, u32 pc);
    void WatchHit(u8 n, u32 addr, u8 size, u32 value, bool write, u8 dma);
    void SwitchStacks(u8 n, u32 oldSP, u32 newSP);
    void PushFrame(u8 n, const CallFrame& frame);
    void ResumeContext(ARM& cpu, u32 pc, bool loaded);
    void UnwindCallers(u8 n, u32 pc, bool thumb, u32 sp, u8 mode, std::vector<CallFrame>& found) const;
    bool ResumeFrame(u8 n, u32 pc, u32 sp, u32 lr, u8 mode, CallFrame& frame) const;
    std::vector<CallFrame> UnwindThread(u8 n, u32 pc, u32 sp, u32 lr, u32 cpsr) const;
    bool StepDone(ARM& cpu, u32 pc, bool jumped) const;
    void TrackEntry(ARM& cpu, u32 pc, u32 prev);
    void SeedBelow(u8 n);
    bool ScanPrologue(u8 n, u32 pc, bool thumb, u32& start, u32& depth, u32& lrBelow) const;
    int CallBefore(u8 n, u32 ret, u32& site, u32& target) const;
    bool UnwindFrame(u8 n, u32 pc, bool thumb, u32 sp, u8 mode, CallFrame& frame, u32& ret) const;
};

}

#endif
