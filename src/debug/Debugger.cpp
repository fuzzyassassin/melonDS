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

#include "Debugger.h"
#include "../ARM.h"
#include "../ARMInterpreter.h"
#include "../NDS.h"

namespace melonDS
{

struct Debugger::ContextGuard
{
    Debugger& D;
    HookContext Kind;
    u8 CPU;
    std::thread::id Thread;

    ContextGuard(Debugger& d, HookContext kind, u8 cpu)
        : D(d), Kind(d.Context), CPU(d.ContextCPU), Thread(d.ContextThread)
    {
        D.Context = kind;
        D.ContextCPU = cpu;
        D.ContextThread = std::this_thread::get_id();
    }
    ~ContextGuard()
    {
        D.Context = Kind;
        D.ContextCPU = CPU;
        D.ContextThread = Thread;
    }
};

constexpr u32 PageShift = 12;
constexpr u32 PageCount = 1u << (32 - PageShift);
constexpr size_t MaxCallDepth = 4096;
constexpr u32 StackMatchWindow = 0x200;
constexpr size_t MaxParkedStacks = 64;
constexpr size_t MaxSeedDepth = 64;
constexpr u32 MaxPrologueScan = 0x1000;
constexpr u32 MaxPrologueOffset = 0x20;
constexpr u32 MaxResumeOffset = 0x1000;
constexpr u32 ThreadInfoMain = 0x027FFFA0;
constexpr u32 ThreadInfoSub = 0x027FFFA4;
constexpr u32 ThreadSavedSP = 0x38;
constexpr u32 ThreadSavedLR = 0x3C;
constexpr u32 ThreadSavedPC = 0x40;
constexpr u32 ThreadNext = 0x68;
constexpr u32 MaxThreads = 64;
constexpr size_t MaxWifiInjected = 256;

static bool IsIndirectJump(u32 instr, bool thumb);
static bool WritesPC(u32 instr, bool thumb);
constexpr u32 MainRAMWords = 0x400000 >> 2;
constexpr u32 PipelineSentinel = 0xE7F0DEF0;
constexpr size_t MaxReturnSkips = 16;

Debugger::Debugger(melonDS::NDS& nds) noexcept : Probes(nds, *this), NDS(nds)
{
}

int Debugger::CallDepth(u8 cpu) const
{
    if (!CallTracking)
        return -1;
    size_t depth = CallStack[cpu & 1].size();
    for (auto& ex : Exceptions[cpu & 1])
        depth += ex.Stack.size();
    return (int)depth;
}

void Debugger::PokeFromHook(u8 cpu, u32 addr, u8 size, u32 value)
{
    ContextGuard guard(*this, HookContext::Hook, cpu);
    Poke(cpu, addr, size, value);
}

void Debugger::Reset()
{
    for (int i = 0; i < 2; i++)
    {
        BreakRequested[i] = false;
        StepRemaining[i] = 0;
        StepModes[i] = StepMode::None;
        PendingValid[i] = false;
        CallStack[i].clear();
        ParkedStacks[i].clear();
        Exceptions[i].clear();
        KnownEntries[i].clear();
        LastSP[i] = {};
        CurPC[i] = 0;
        SeedPending[i] = false;
        ReturnSkips[i].clear();
    }
    TraceHead = 0;
    TraceWrapped = false;
    if (!LastWriter.empty())
        std::fill(LastWriter.begin(), LastWriter.end(), 0);
    Probes.Reset();
    Rearm();
}

int Debugger::AddBreakpoint(u8 cpuMask, u32 addr, BreakCallback callback)
{
    Breakpoints.push_back({NextID, cpuMask, true, addr & ~1u, 0, std::move(callback)});
    RebuildPages();
    return NextID++;
}

int Debugger::AddWatchpoint(u8 cpuMask, u32 start, u32 size, u8 flags, BreakCallback callback)
{
    if (size == 0 || !(flags & (Watch_Read | Watch_Write)))
        return -1;
    u32 end = start + size - 1;
    if (end < start)
        end = 0xFFFFFFFF;
    Watchpoints.push_back({NextID, cpuMask, flags, true, start, end, 0, std::move(callback)});
    RebuildPages();
    return NextID++;
}

bool Debugger::Remove(int id)
{
    auto bp = std::find_if(Breakpoints.begin(), Breakpoints.end(), [id](const Breakpoint& b) { return b.ID == id; });
    if (bp != Breakpoints.end())
    {
        Breakpoints.erase(bp);
        RebuildPages();
        return true;
    }
    auto wp = std::find_if(Watchpoints.begin(), Watchpoints.end(), [id](const Watchpoint& w) { return w.ID == id; });
    if (wp != Watchpoints.end())
    {
        Watchpoints.erase(wp);
        RebuildPages();
        return true;
    }
    auto hook = std::find_if(WifiHooks.begin(), WifiHooks.end(), [id](const auto& h) { return h.first == id; });
    if (hook != WifiHooks.end())
    {
        WifiHooks.erase(hook);
        return true;
    }
    return false;
}

bool Debugger::IsHook(int id) const
{
    for (auto& b : Breakpoints)
        if (b.ID == id)
            return bool(b.Callback);
    for (auto& w : Watchpoints)
        if (w.ID == id)
            return bool(w.Callback);
    for (auto& h : WifiHooks)
        if (h.first == id)
            return true;
    return false;
}

int Debugger::AddWifiHook(WifiCallback callback)
{
    WifiHooks.push_back({NextID, std::move(callback)});
    return NextID++;
}

bool Debugger::RunWifiHooks(WifiFrame& frame)
{
    auto hooks = WifiHooks;
    for (auto& [id, callback] : hooks)
        if (!callback(frame))
            return false;
    return true;
}

void Debugger::InjectWifi(std::vector<u8> frame, u8 rate)
{
    if (WifiInjected.size() >= MaxWifiInjected)
        WifiInjected.pop_front();
    WifiInjected.push_back({false, rate, 0, 0, std::move(frame)});
}

bool Debugger::TakeWifiInjection(WifiFrame& frame)
{
    if (WifiInjected.empty())
        return false;
    frame = std::move(WifiInjected.front());
    WifiInjected.pop_front();
    return true;
}

bool Debugger::SetEnabled(int id, bool enabled)
{
    for (auto& b : Breakpoints)
        if (b.ID == id) { b.Enabled = enabled; RebuildPages(); return true; }
    for (auto& w : Watchpoints)
        if (w.ID == id) { w.Enabled = enabled; RebuildPages(); return true; }
    return false;
}

void Debugger::ClearAll()
{
    Breakpoints.clear();
    Watchpoints.clear();
    RebuildPages();
}

void Debugger::SetBreakHandler(std::function<void(const BreakEvent&)> handler)
{
    Handler = std::move(handler);
}

void Debugger::SetServiceHandler(std::function<void()> handler)
{
    ServiceHandler = std::move(handler);
}

void Debugger::RequestService()
{
    ServiceRequested = true;
    Armed[0] = true;
    Armed[1] = true;
}

void Debugger::RequestBreak(u8 cpuMask)
{
    for (int i = 0; i < 2; i++)
        if (cpuMask & (1 << i))
        {
            BreakRequested[i] = true;
            Armed[i] = true;
        }
}

void Debugger::CancelBreakRequests(bool steps)
{
    BreakRequested[0] = false;
    BreakRequested[1] = false;
    if (steps)
        StepRemaining = {};
    Rearm();
}

void Debugger::Step(u8 cpu, u32 count)
{
    u8 n = cpu & 1;
    // Parked inside this CPU's instruction hook, the instruction at the break has passed its hook already and
    // runs on resume, so it is the first of the count. Otherwise the next hook call is for the first one, and
    // the countdown reaching zero there would park before anything ran.
    if (count && InstrBreakCPU != n && count != 0xFFFFFFFF)
        count++;
    StepRemaining[n] = count;
    Rearm();
}

void Debugger::OnStateLoaded()
{
    for (int i = 0; i < 2; i++)
    {
        CallStack[i].clear();
        ParkedStacks[i].clear();
        Exceptions[i].clear();
        LastSP[i] = {};
        LastMode[i] = 0;
        PrevInstr[i] = 0;
        PrevThumb[i] = false;
        Prev2Instr[i] = 0;
        JumpTarget[i] = 0;
        PendingValid[i] = false;
        // rebuild the stacks from the loaded CPU state: the frames below the first call are unwound from
        // the stack again (see SeedBelow), and calls that return into a hook's memory.execute are forgotten
        SeedPending[i] = CallTracking;
        ReturnSkips[i].clear();
    }
    ARM* cpus[2] = {NDS.ARM9, NDS.ARM7};
    for (int i = 0; i < 2; i++)
        CurPC[i] = cpus[i]->R[15] - ((cpus[i]->CPSR & 0x20) ? 2 : 4);
    if (!LastWriter.empty())
        std::fill(LastWriter.begin(), LastWriter.end(), 0);
    CoverageLastBits = {};
    Probes.OnStateLoaded();
    Rearm();
}

void Debugger::StepTo(u8 cpu, StepMode mode)
{
    u8 n = cpu & 1;
    StepModes[n] = mode;
    StepDepth[n] = CallStack[n].size();
    StepFrame[n] = CallStack[n].empty() ? CallFrame {} : CallStack[n].back();
    StepExceptions[n] = Exceptions[n].size();
    Rearm();
}

bool Debugger::StepDone(ARM& cpu, u32 pc, bool jumped) const
{
    u8 n = cpu.Num;
    if (StepModes[n] == StepMode::TakenBranch)
        return jumped;
    bool thumb = cpu.CPSR & 0x20;
    if (!WritesPC(cpu.NextInstr[0], thumb) || CallReturn(n, pc, thumb, cpu.R[14]))
        return false;
    const auto& stack = CallStack[n];
    if (Exceptions[n].size() != StepExceptions[n] || stack.size() > StepDepth[n])
        return false;
    return stack.size() < StepDepth[n] || StepDepth[n] == 0
        || (stack.back().Return == StepFrame[n].Return && stack.back().SP == StepFrame[n].SP);
}

u32 Debugger::CallReturn(u8 cpu, u32 pc, bool thumb, u32 lr) const
{
    u32 next = pc + (thumb ? 2 : 4);
    u32 instr = Peek(cpu, pc, thumb ? 2 : 4);
    if (thumb)
    {
        if ((instr & 0xF800) == 0xF000 && (Peek(cpu, pc + 2, 2) & 0xE800) == 0xE800)
            return pc + 4;
        if ((instr & 0xFF87) == 0x4780 || (instr & 0xFF00) == 0xDF00)
            return next;
    }
    else if ((instr & 0xFE000000) == 0xFA000000 || ((instr >> 28) != 0xF && ((instr & 0x0F000000) == 0x0B000000
        || (instr & 0x0F000000) == 0x0F000000 || (instr & 0x0FFFFFF0) == 0x012FFF30)))
        return next;
    return IsIndirectJump(instr, thumb) && (lr & ~1u) == next ? next : 0;
}

void Debugger::SetTrace(u8 cpuMask, u32 start, u32 end, u32 capacity)
{
    TraceMask = capacity ? cpuMask : 0;
    TraceStart = start;
    TraceEnd = end;
    Trace.assign(TraceMask ? capacity : 0, {});
    TraceHead = 0;
    TraceWrapped = false;
    Rearm();
}

std::vector<TraceEntry> Debugger::DrainTrace()
{
    std::vector<TraceEntry> out;
    if (TraceWrapped)
    {
        out.insert(out.end(), Trace.begin() + TraceHead, Trace.end());
        out.insert(out.end(), Trace.begin(), Trace.begin() + TraceHead);
    }
    else
        out.insert(out.end(), Trace.begin(), Trace.begin() + TraceHead);
    TraceHead = 0;
    TraceWrapped = false;
    return out;
}

void Debugger::SetCallTracking(bool enabled)
{
    CallTracking = enabled;
    for (int i = 0; i < 2; i++)
    {
        if (!enabled)
        {
            CallStack[i].clear();
            ParkedStacks[i].clear();
            Exceptions[i].clear();
            KnownEntries[i].clear();
            LastSP[i] = {};
        }
        SeedPending[i] = enabled;
    }
    Rearm();
}

std::vector<CallFrame> Debugger::GetCallStack(u8 cpu) const
{
    std::vector<CallFrame> out;
    for (auto& ex : Exceptions[cpu & 1])
        out.insert(out.end(), ex.Stack.begin(), ex.Stack.end());
    out.insert(out.end(), CallStack[cpu & 1].begin(), CallStack[cpu & 1].end());
    return out;
}

std::vector<std::pair<u32, std::vector<CallFrame>>> Debugger::GetThreadStacks(u8 cpu) const
{
    u8 n = cpu & 1;
    std::vector<std::pair<u32, std::vector<CallFrame>>> out(ParkedStacks[n].begin(), ParkedStacks[n].end());
    u32 info = Peek(n, n ? ThreadInfoSub : ThreadInfoMain, 4);
    if ((info >> 24) != 0x02 && (info >> 24) != 0x03)
        return out;
    u32 current = Peek(n, info + 4, 4);
    u32 thread = Peek(n, info + 8, 4);
    for (u32 i = 0; i < MaxThreads && ((thread >> 24) == 0x02 || (thread >> 24) == 0x03); i++)
    {
        if (thread != current)
        {
            u32 cpsr = Peek(n, thread, 4);
            u32 sp = Peek(n, thread + ThreadSavedSP, 4);
            u32 lr = Peek(n, thread + ThreadSavedLR, 4);
            u32 pc = Peek(n, thread + ThreadSavedPC, 4) - 4;
            auto tracked = std::find_if(out.begin(), out.end(), [sp](const auto& s) { return s.first == sp; });
            CallFrame top;
            if (tracked == out.end())
                out.push_back({sp, UnwindThread(n, pc, sp, lr, cpsr)});
            else if (!tracked->second.empty() && tracked->second.back().SP == sp
                && ResumeFrame(n, pc, sp, lr, cpsr & 0x1F, top))
                tracked->second.back() = top;
        }
        thread = Peek(n, thread + ThreadNext, 4);
    }
    return out;
}

std::unordered_map<u32, u64> Debugger::DrainCallCounts(u8 cpu)
{
    std::unordered_map<u32, u64> out;
    out.swap(CallCounts[cpu & 1]);
    return out;
}

void Debugger::SetCoverage(bool enabled)
{
    Coverage = enabled;
    Rearm();
}

std::vector<u32> Debugger::DrainCoverage(u8 cpu)
{
    std::vector<u32> out;
    for (auto& [page, bits] : CoveragePages[cpu & 1])
        for (u32 w = 0; w < bits.size(); w++)
            for (u64 b = bits[w]; b; b &= b - 1)
                out.push_back((page << PageShift) | (((w << 6) | __builtin_ctzll(b)) << 1));
    std::sort(out.begin(), out.end());
    CoveragePages[cpu & 1].clear();
    CoverageLastBits[cpu & 1] = nullptr;
    return out;
}

void Debugger::SetLastWriter(bool enabled)
{
    if (enabled)
        LastWriter.assign(MainRAMWords, 0);
    else
        LastWriter.clear();
    Rearm();
}

u32 Debugger::GetLastWriter(u32 addr) const
{
    if (LastWriter.empty() || (addr >> 24) != 0x02)
        return 0;
    return LastWriter[(addr & 0x3FFFFF) >> 2];
}

u32 Debugger::Peek(u8 cpu, u32 addr, u8 size) const
{
    addr &= ~(u32)(size - 1);
    if (cpu == 0)
    {
        ARMv5& arm9 = *NDS.ARM9;
        const u8* tcm = nullptr;
        if (addr < arm9.ITCMSize)
            tcm = &arm9.ITCM[addr & (ITCMPhysicalSize - 1)];
        else if ((addr & arm9.DTCMMask) == arm9.DTCMBase)
            tcm = &arm9.DTCM[addr & (DTCMPhysicalSize - 1)];
        if (tcm)
        {
            if (size == 1) return *tcm;
            if (size == 2) return *(const u16*)tcm;
            return *(const u32*)tcm;
        }
    }
    if ((addr >> 24) == 0x04)
        return 0;
    if (cpu == 0)
    {
        if (size == 1) return NDS.ARM9Read8(addr);
        if (size == 2) return NDS.ARM9Read16(addr);
        return NDS.ARM9Read32(addr);
    }
    if (size == 1) return NDS.ARM7Read8(addr);
    if (size == 2) return NDS.ARM7Read16(addr);
    return NDS.ARM7Read32(addr);
}

bool Debugger::Poke(u8 cpu, u32 addr, u8 size, u32 value)
{
    addr &= ~(u32)(size - 1);
    if (cpu == 0)
    {
        ARMv5& arm9 = *NDS.ARM9;
        u8* tcm = nullptr;
        if (addr < arm9.ITCMSize)
            tcm = &arm9.ITCM[addr & (ITCMPhysicalSize - 1)];
        else if ((addr & arm9.DTCMMask) == arm9.DTCMBase)
            tcm = &arm9.DTCM[addr & (DTCMPhysicalSize - 1)];
        if (tcm)
        {
            if (size == 1) *tcm = value;
            else if (size == 2) *(u16*)tcm = value;
            else *(u32*)tcm = value;
            SyncPipelines(addr, size);
            return true;
        }
    }
    if ((addr >> 24) == 0x04)
        return false;
    if (cpu == 0)
    {
        if (size == 1) NDS.ARM9Write8(addr, value);
        else if (size == 2) NDS.ARM9Write16(addr, value);
        else NDS.ARM9Write32(addr, value);
    }
    else
    {
        if (size == 1) NDS.ARM7Write8(addr, value);
        else if (size == 2) NDS.ARM7Write16(addr, value);
        else NDS.ARM7Write32(addr, value);
    }
    SyncPipelines(addr, size);
    return true;
}

const char* Debugger::RegionName(u8 cpu, u32 addr) const
{
    if (cpu == 0)
    {
        ARMv5& arm9 = *NDS.ARM9;
        if (addr < arm9.ITCMSize)
            return "ITCM";
        if ((addr & arm9.DTCMMask) == arm9.DTCMBase)
            return "DTCM";
        if (addr >= 0xFFFF0000)
            return "ARM9 BIOS";
    }
    switch (addr >> 24)
    {
    case 0x00: return cpu ? "ARM7 BIOS" : "unmapped memory";
    case 0x02: return "main RAM";
    case 0x03: return cpu ? "WRAM" : "shared WRAM";
    case 0x04: return "I/O";
    case 0x05: return cpu ? "unmapped memory" : "palette";
    case 0x06: return "VRAM";
    case 0x07: return cpu ? "unmapped memory" : "OAM";
    case 0x08: case 0x09: return "the GBA ROM area";
    case 0x0A: return "GBA SRAM";
    default: return "unmapped memory";
    }
}

bool Debugger::WriteMemory(u8 cpu, u32 addr, const u8* data, u32 len, std::string& error)
{
    cpu &= 1;
    auto fail = [&](u32 a, const std::string& why) {
        char where[16];
        snprintf(where, sizeof(where), "0x%08X", a);
        error = std::string(RegionName(cpu, a)) + " at " + where + why;
        return false;
    };
    for (u32 off = 0; off < len;)
    {
        u32 a = addr + off;
        bool tcm = cpu == 0 && (a < NDS.ARM9->ITCMSize || (a & NDS.ARM9->DTCMMask) == NDS.ARM9->DTCMBase);
        u8 top = a >> 24;
        if (!tcm && top == 0x04)
            return fail(a, " cannot be written by the debugger (I/O writes have side effects)");
        if (!tcm && (top == 0x08 || top == 0x09 || (cpu == 0 && a >= 0xFFFF0000) || (cpu == 1 && top == 0x00)))
            return fail(a, " is read-only");
        if (!tcm && cpu == 0 && top >= 0x05 && top <= 0x07)
        {
            // the ARM9 bus drops 8-bit writes here: write the halfword with the neighbouring byte kept
            u32 h = a & ~1u;
            u16 v = Peek(cpu, h, 2);
            for (u32 b = 0; b < 2; b++)
                if (h + b - addr < len)
                    v = (v & ~(0xFF << (8 * b))) | (data[h + b - addr] << (8 * b));
            Poke(cpu, h, 2, v);
            if (Peek(cpu, h, 2) != v)
                return fail(a, top == 0x06 ? " did not take the write (no VRAM bank mapped there?)"
                                           : " did not take the write (its 2D engine powered off?)");
            off = h + 2 - addr;
            continue;
        }
        Poke(cpu, a, 1, data[off]);
        if (Peek(cpu, a, 1) != data[off])
            return fail(a, top == 0x0A ? " did not take the write (no GBA cart SRAM?)" : " did not take the write");
        off++;
    }
    return true;
}

bool Debugger::OnContextThread() const
{
    return Context != HookContext::None && ContextThread == std::this_thread::get_id();
}

int Debugger::HookedCPU() const
{
    return Context == HookContext::Hook && OnContextThread() ? ContextCPU : -1;
}

u32 Debugger::Canonical(u8 cpu, u32 addr) const
{
    if ((addr >> 24) == 0x02)
        return 0x02000000 | (addr & NDS.MainRAMMask);
    if (cpu == 0 && addr < NDS.ARM9->ITCMSize)
        return addr & (ITCMPhysicalSize - 1);
    return addr;
}

bool Debugger::PipelineCurrent(ARM& cpu) const
{
    u8 n = cpu.Num;
    if (!(cpu.CPSR & 0x20))
        return cpu.NextInstr[0] == Peek(n, cpu.R[15] - 4, 4) && cpu.NextInstr[1] == Peek(n, cpu.R[15], 4);
    u32 pc = cpu.R[15] - 2;
    if ((cpu.NextInstr[0] & 0xFFFF) != Peek(n, pc, 2))
        return false;
    if (n == 0 && (pc & 2))
        return cpu.NextInstr[1] == Peek(n, pc + 2, 4);
    return (cpu.NextInstr[1] & 0xFFFF) == Peek(n, pc + 2, 2);
}

void Debugger::SyncPipelines(u32 addr, u32 size)
{
    if (!size || !OnContextThread() || NDS.IsJITEnabled())
        return;
    for (u8 n = 0; n < 2; n++)
    {
        if (MidInstr[n])
            continue;
        ARM& cpu = n ? (ARM&)*NDS.ARM7 : (ARM&)*NDS.ARM9;
        u32 window = Canonical(n, cpu.R[15] - 4);
        u32 start = Canonical(n, addr);
        if (start + size > window && start < window + 8 && !PipelineCurrent(cpu))
            cpu.FillPipeline();
    }
}

static bool IsLinkBranch(u32 op, bool thumb)
{
    if (thumb)
        return (op & 0xF800) == 0xF800 || (op & 0xF800) == 0xE800 || (op & 0xFF87) == 0x4780;
    return ((op >> 28) != 0xF && (op & 0x0F000000) == 0x0B000000) || (op & 0xFE000000) == 0xFA000000
        || (op & 0x0FFFFFF0) == 0x012FFF30;
}

void Debugger::Redirected(ARM& cpu, bool thumb, u32 op, u32 lr, u32 sp, u8 callerMode, const ReturnSkip& entry)
{
    u8 n = cpu.Num;
    u32 pc = entry.PC;
    PrevInstr[n] = op;
    PrevThumb[n] = thumb;
    u32 vector = cpu.R[15] - 4 - cpu.ExceptionBase;
    u8 mode = cpu.CPSR & 0x1F;
    auto expectReturn = [&] {
        if (ReturnSkips[n].size() >= MaxReturnSkips)
            ReturnSkips[n].erase(ReturnSkips[n].begin());
        ReturnSkip skip = entry;
        skip.SP = sp;
        skip.Mode = callerMode;
        ReturnSkips[n].push_back(skip);
    };
    if ((vector == 0x04 && mode == 0x1B) || (vector == 0x08 && mode == 0x13))
    {
        u32 old = cpu.R[14];
        cpu.R[14] = pc;
        if (!Exceptions[n].empty() && Exceptions[n].back().Return == old)
        {
            Exceptions[n].back().Return = pc;
            if (!CallStack[n].empty())
                CallStack[n].front().Site = CallStack[n].front().Return = pc;
        }
        expectReturn();
        return;
    }
    if (!IsLinkBranch(op, thumb) || cpu.R[14] == lr)
        return;
    u32 old = cpu.R[14] & ~1u;
    cpu.R[14] = pc | (thumb ? 1 : 0);
    if (!CallStack[n].empty() && CallStack[n].back().Return == old)
        CallStack[n].back().Return = pc;
    expectReturn();
}

bool Debugger::ReturnedToHook(ARM& cpu, u32 pc)
{
    auto& skips = ReturnSkips[cpu.Num];
    if (skips.empty())
        return false;
    ReturnSkip& top = skips.back();
    if (pc != top.PC)
    {
        top.Left = true;
        return false;
    }
    if (!top.Left || cpu.R[13] != top.SP || (cpu.CPSR & 0x1F) != top.Mode)
        return false;
    for (int r = 0; r < 4; r++)
        cpu.R[r] = top.Args[r];
    cpu.R[12] = top.R12;
    cpu.R[14] = top.LR;
    cpu.CPSR = (cpu.CPSR & 0x07FFFFFF) | top.Flags;
    skips.pop_back();
    return true;
}

ExecResult Debugger::Execute(u8 cpu, const std::vector<u32>& ops)
{
    u8 n = cpu & 1;
    if (HookedCPU() != n || MidInstr[n] || NDS.IsJITEnabled())
        return ExecResult::Refused;
    ARM& arm = n ? (ARM&)*NDS.ARM7 : (ARM&)*NDS.ARM9;
    bool thumb = arm.CPSR & 0x20;
    u32 step = thumb ? 2 : 4;
    u32 pc = arm.R[15] - step;
    u32 cur = arm.CurInstr;
    ReturnSkip entry {pc, 0, 0, false, {arm.R[0], arm.R[1], arm.R[2], arm.R[3]}, arm.R[12], arm.R[14], arm.CPSR & 0xF8000000};
    std::vector<u32> seq;
    for (u32 op : ops)
    {
        seq.push_back(thumb ? op & 0xFFFF : op);
        if (thumb && op > 0xFFFF)
            seq.push_back(op >> 16);
    }
    for (u32 op : seq)
    {
        u32 lr = arm.R[14];
        u32 sp = arm.R[13];
        u8 mode = arm.CPSR & 0x1F;
        arm.R[15] = pc + 2 * step;
        arm.CurInstr = op;
        arm.NextInstr[0] = arm.NextInstr[1] = PipelineSentinel;
        MidInstr[n]++;
        if (thumb)
            ARMInterpreter::THUMBInstrTable[(op >> 6) & 0x3FF](&arm);
        else if (arm.CheckCondition(op >> 28))
            ARMInterpreter::ARMInstrTable[((op >> 4) & 0xF) | ((op >> 16) & 0xFF0)](&arm);
        else if (n == 0 && (op & 0xFE000000) == 0xFA000000)
            ARMInterpreter::A_BLX_IMM(&arm);
        else
            arm.AddCycles_C();
        MidInstr[n]--;
        if (arm.R[15] != pc + 2 * step || arm.NextInstr[0] != PipelineSentinel || arm.NextInstr[1] != PipelineSentinel)
        {
            arm.CurInstr = cur;
            Redirected(arm, thumb, op, lr, sp, mode, entry);
            return ExecResult::Branched;
        }
    }
    arm.R[15] = pc + step;
    arm.CurInstr = cur;
    arm.FillPipeline();
    return ExecResult::Continued;
}

void Debugger::Rearm()
{
    CallsArmed = CallTracking;
    for (int i = 0; i < 2; i++)
    {
        bool exec = false;
        bool data = !LastWriter.empty();
        for (auto& b : Breakpoints)
            exec |= b.Enabled && (b.CPUMask & (1 << i));
        for (auto& w : Watchpoints)
            data |= w.Enabled && (w.CPUMask & (1 << i));
        // the memory log needs no instruction hook: it takes the pc from R15 (Debugger::OnData)
        DataArmed[i] = data || Probes.DataArmed(i);
        Armed[i] = exec || data || Probes.InstrArmed(i) || CallTracking || Coverage || (TraceMask & (1 << i))
            || StepRemaining[i] || StepModes[i] != StepMode::None || PendingValid[i] || BreakRequested[i] || ServiceRequested;
    }
}

void Debugger::RebuildPages()
{
    for (int i = 0; i < 2; i++)
    {
        ExecPages[i].clear();
        WatchPages[i].clear();
        for (auto& b : Breakpoints)
        {
            if (!b.Enabled || !(b.CPUMask & (1 << i)))
                continue;
            if (ExecPages[i].empty())
                ExecPages[i].assign(PageCount, 0);
            ExecPages[i][b.Addr >> PageShift] = 1;
        }
        for (auto& w : Watchpoints)
        {
            if (!w.Enabled || !(w.CPUMask & (1 << i)))
                continue;
            if (WatchPages[i].empty())
                WatchPages[i].assign(PageCount, 0);
            for (u64 p = w.Start >> PageShift; p <= (w.End >> PageShift); p++)
                WatchPages[i][p] = 1;
        }
    }
    Rearm();
}

void Debugger::Raise(const BreakEvent& ev)
{
    if (!Handler || InBreak)
        return;
    StepModes[ev.CPU & 1] = StepMode::None;
    InBreak = true;
    Handler(ev);
    InBreak = false;
    Rearm();
}

void Debugger::SwitchStacks(u8 n, u32 oldSP, u32 newSP)
{
    auto& parked = ParkedStacks[n];
    if (!CallStack[n].empty())
    {
        if (parked.size() >= MaxParkedStacks)
            parked.erase(parked.begin());
        parked[oldSP] = std::move(CallStack[n]);
    }
    CallStack[n].clear();
    auto best = parked.end();
    u32 bestDist = StackMatchWindow + 1;
    for (auto it = parked.lower_bound(newSP > StackMatchWindow ? newSP - StackMatchWindow : 0);
         it != parked.end() && it->first <= newSP + StackMatchWindow; ++it)
    {
        u32 dist = it->first > newSP ? it->first - newSP : newSP - it->first;
        if (dist < bestDist)
        {
            best = it;
            bestDist = dist;
        }
    }
    SeedPending[n] = best == parked.end();
    if (best != parked.end())
    {
        CallStack[n] = std::move(best->second);
        parked.erase(best);
    }
}

static bool IsExceptionMode(u8 mode)
{
    return mode != 0x10 && mode != 0x1F;
}

static u32 ThreadSP(const ARM& cpu)
{
    switch (cpu.CPSR & 0x1F)
    {
    case 0x11: return cpu.R_FIQ[5];
    case 0x12: return cpu.R_IRQ[0];
    case 0x13: return cpu.R_SVC[0];
    case 0x17: return cpu.R_ABT[0];
    case 0x1B: return cpu.R_UND[0];
    default: return cpu.R[13];
    }
}

static u32 Ror(u32 v, u32 r)
{
    return r ? (v >> r) | (v << (32 - r)) : v;
}

static u32 SignExtend(u32 v, int bits)
{
    return (u32)((s32)(v << (32 - bits)) >> (32 - bits));
}

static bool IsPushLR(u32 instr, bool thumb)
{
    if (thumb)
        return (instr & 0xFF00) == 0xB500;
    return (instr & 0xFFFF4000) == 0xE92D4000 || instr == 0xE52DE004;
}

static bool IsIndirectJump(u32 instr, bool thumb)
{
    if (thumb)
    {
        u16 t = instr & 0xFFFF;
        return ((t & 0xFF87) == 0x4700 || (t & 0xFF87) == 0x4687) && ((t >> 3) & 0xF) != 14;
    }
    u32 rn = (instr >> 16) & 0xF;
    if ((instr & 0x0FFFFFF0) == 0x012FFF10 || (instr & 0x0FEFFFF0) == 0x01A0F000)
        return (instr & 0xF) != 14;
    return (instr & 0x0C10F000) == 0x0410F000 && rn != 13 && rn != 15;
}

static bool WritesPC(u32 instr, bool thumb)
{
    if (thumb)
    {
        u16 t = instr & 0xFFFF;
        return (t & 0xF000) == 0xD000 || (t & 0xE000) == 0xE000 || (t & 0xFF00) == 0x4700
            || (t & 0xFF00) == 0xBD00 || (t & 0xFF87) == 0x4687 || (t & 0xFF87) == 0x4487;
    }
    if ((instr & 0x0E000000) == 0x0A000000 || (instr & 0x0FFFFFD0) == 0x012FFF10)
        return true;
    if ((instr >> 28) == 0xF)
        return false;
    if ((instr & 0x0F000000) == 0x0F000000 || (instr & 0x0E108000) == 0x08108000 || (instr & 0x0C10F000) == 0x0410F000)
        return true;
    bool misc = (instr & 0x0F900000) == 0x01000000 && !(instr & (1 << 20));
    bool multiply = (instr & 0x02000090) == 0x90;
    return (instr & 0x0C00F000) == 0x0000F000 && !misc && !multiply;
}

static bool EndsPrologue(u32 instr, bool thumb)
{
    if (thumb)
    {
        u16 t = instr & 0xFFFF;
        return (t & 0xF000) == 0xD000 || (t & 0xE000) == 0xE000 || (t & 0xFF00) == 0x4700
            || (t & 0xFF00) == 0xBD00 || (t & 0xFF80) == 0xB000;
    }
    return (instr & 0x0E000000) == 0x0A000000 || (instr & 0x0FFFFFD0) == 0x012FFF10
        || (instr & 0x0FFF0000) == 0x08BD0000 || (instr & 0x0FFFF000) == 0x028DD000;
}

bool Debugger::ScanPrologue(u8 n, u32 pc, bool thumb, u32& start, u32& depth, u32& lrBelow) const
{
    u32 step = thumb ? 2 : 4;
    u32 a = pc & ~(step - 1);
    u32 i = 0;
    while (i < MaxPrologueScan && !IsPushLR(Peek(n, a, step), thumb))
    {
        a -= step;
        i++;
    }
    if (i == MaxPrologueScan)
        return false;
    start = a;
    depth = 0;
    lrBelow = 0;
    std::array<u32, 16> lit {};
    u32 known = 0;
    for (; a < pc; a += step)
    {
        u32 instr = Peek(n, a, step);
        if (thumb)
        {
            if ((instr & 0xFE00) == 0xB400)
            {
                if (instr & 0x100)
                    lrBelow = depth + 4;
                depth += 4 * __builtin_popcount(instr & 0x1FF);
            }
            else if ((instr & 0xFF80) == 0xB080)
                depth += (instr & 0x7F) << 2;
            else if ((instr & 0xF800) == 0x4800)
            {
                lit[(instr >> 8) & 7] = Peek(n, ((a + 4) & ~3u) + ((instr & 0xFF) << 2), 4);
                known |= 1u << ((instr >> 8) & 7);
            }
            else if ((instr & 0xFF87) == 0x4485 && (known & (1u << ((instr >> 3) & 0xF))))
                depth -= lit[(instr >> 3) & 0xF];
            else if (EndsPrologue(instr, true))
                break;
        }
        else
        {
            if ((instr & 0xFFFF0000) == 0xE92D0000)
            {
                depth += 4 * __builtin_popcount(instr & 0xFFFF);
                if (instr & 0x4000)
                    lrBelow = depth - 4 * __builtin_popcount(instr & 0x3FFF);
            }
            else if ((instr & 0xFFFF0FFF) == 0xE52D0004)
            {
                depth += 4;
                if (((instr >> 12) & 0xF) == 14)
                    lrBelow = depth;
            }
            else if ((instr & 0xFFFFF000) == 0xE24DD000)
                depth += Ror(instr & 0xFF, ((instr >> 8) & 0xF) * 2);
            else if ((instr & 0xFFFF0000) == 0xE59F0000)
            {
                lit[(instr >> 12) & 0xF] = Peek(n, a + 8 + (instr & 0xFFF), 4);
                known |= 1u << ((instr >> 12) & 0xF);
            }
            else if ((instr & 0xFFFFFFF0) == 0xE04DD000 && (known & (1u << (instr & 0xF))))
                depth += lit[instr & 0xF];
            else if (EndsPrologue(instr, false))
                break;
        }
    }
    return lrBelow != 0;
}

int Debugger::CallBefore(u8 n, u32 ret, u32& site, u32& target) const
{
    if (ret & 1)
    {
        u32 r = ret & ~1u;
        u32 lo = Peek(n, r - 2, 2);
        site = r - 2;
        if ((lo & 0xFF87) == 0x4780)
            return 2;
        u32 hi = Peek(n, r - 4, 2);
        if ((hi & 0xF800) != 0xF000)
            return 0;
        u32 dest = r + (SignExtend(hi & 0x7FF, 11) << 12) + ((lo & 0x7FF) << 1);
        if ((lo & 0xF800) == 0xF800)
            target = dest;
        else if ((lo & 0xF800) == 0xE800)
            target = dest & ~3u;
        else
            return 0;
        return 1;
    }
    u32 instr = Peek(n, ret - 4, 4);
    site = ret - 4;
    if ((instr >> 28) != 0xF && (instr & 0x0F000000) == 0x0B000000)
    {
        target = ret + 4 + (SignExtend(instr & 0xFFFFFF, 24) << 2);
        return 1;
    }
    if ((instr & 0xFE000000) == 0xFA000000)
    {
        target = ret + 4 + (SignExtend(instr & 0xFFFFFF, 24) << 2) + ((instr >> 23) & 2);
        return 1;
    }
    if ((instr & 0x0FFFFFF0) == 0x012FFF30 || (Peek(n, ret - 8, 4) & 0x0FFFFFFF) == 0x01A0E00F)
        return 2;
    return 0;
}

bool Debugger::UnwindFrame(u8 n, u32 pc, bool thumb, u32 sp, u8 mode, CallFrame& frame, u32& ret) const
{
    u32 start, depth, lrBelow, site, target;
    if (!ScanPrologue(n, pc, thumb, start, depth, lrBelow))
        return false;
    u32 cfa = sp + depth;
    ret = Peek(n, cfa - lrBelow, 4);
    int kind = CallBefore(n, ret, site, target);
    if (kind == 0)
        return false;
    if (kind == 1)
    {
        target &= ~1u;
        if (target > start || start - target > MaxPrologueOffset)
            return false;
        start = target;
    }
    frame = {site, start, ret & ~1u, cfa, mode, false};
    return true;
}

void Debugger::SeedBelow(u8 n)
{
    auto& stack = CallStack[n];
    const CallFrame& bottom = stack.front();
    bool thumb = bottom.Return - bottom.Site == 2;
    u32 site, target;
    if (bottom.Exception || IsExceptionMode(bottom.Mode) || !CallBefore(n, bottom.Return | thumb, site, target))
        return;
    std::vector<CallFrame> found;
    UnwindCallers(n, bottom.Site, thumb, bottom.SP, bottom.Mode, found);
    for (auto& frame : found)
        KnownEntries[n].insert(frame.Entry);
    stack.insert(stack.begin(), found.rbegin(), found.rend());
    if (stack.size() > MaxCallDepth)
        stack.erase(stack.begin(), stack.begin() + (stack.size() - MaxCallDepth));
}

void Debugger::UnwindCallers(u8 n, u32 pc, bool thumb, u32 sp, u8 mode, std::vector<CallFrame>& found) const
{
    u32 ret;
    CallFrame frame;
    while (found.size() < MaxSeedDepth && UnwindFrame(n, pc, thumb, sp, mode, frame, ret) && frame.SP > sp)
    {
        found.push_back(frame);
        pc = frame.Site;
        sp = frame.SP;
        thumb = ret & 1;
    }
}

bool Debugger::ResumeFrame(u8 n, u32 pc, u32 sp, u32 lr, u8 mode, CallFrame& frame) const
{
    u32 site, target;
    if (CallBefore(n, lr, site, target) != 1)
        return false;
    target &= ~1u;
    if (pc < target || pc - target >= MaxResumeOffset)
        return false;
    frame = {site, target, lr & ~1u, sp, mode, false};
    return true;
}

std::vector<CallFrame> Debugger::UnwindThread(u8 n, u32 pc, u32 sp, u32 lr, u32 cpsr) const
{
    std::vector<CallFrame> found;
    u8 mode = cpsr & 0x1F;
    bool thumb = cpsr & 0x20;
    CallFrame frame;
    u32 ret;
    if (UnwindFrame(n, pc, thumb, sp, mode, frame, ret))
        thumb = ret & 1;
    else if (ResumeFrame(n, pc, sp, lr, mode, frame))
        thumb = lr & 1;
    else
        return found;
    found.push_back(frame);
    UnwindCallers(n, frame.Site, thumb, frame.SP, mode, found);
    std::reverse(found.begin(), found.end());
    return found;
}

void Debugger::ResumeContext(ARM& cpu, u32 pc, bool loaded)
{
    u8 n = cpu.Num;
    auto& stack = CallStack[n];
    u32 sp = cpu.R[13];
    CallFrame frame;
    if (!ResumeFrame(n, pc, sp, cpu.R[14], cpu.CPSR & 0x1F, frame))
        return;
    if (!stack.empty() && stack.back().SP == sp)
        stack.back() = frame;
    else if (stack.empty() && loaded)
        PushFrame(n, frame);
}

static CallFrame Called(const ARM& cpu, u32 site, u32 entry, u32 ret, u32 sp, u8 mode)
{
    CallFrame frame {site, entry, ret, sp, mode, false, true};
    std::copy(cpu.R, cpu.R + 4, frame.Args);
    return frame;
}

void Debugger::PushFrame(u8 n, const CallFrame& frame)
{
    auto& stack = CallStack[n];
    if (stack.size() >= MaxCallDepth)
        stack.erase(stack.begin());
    stack.push_back(frame);
    CallCounts[n][frame.Entry]++;
    if (Probes.Tagging())
        Probes.CountCall(n, frame.Entry);
    KnownEntries[n].insert(frame.Entry);
    if (SeedPending[n] && Exceptions[n].empty() && !IsExceptionMode(frame.Mode))
    {
        SeedPending[n] = false;
        SeedBelow(n);
    }
}

void Debugger::TrackEntry(ARM& cpu, u32 pc, u32 prev)
{
    u8 n = cpu.Num;
    auto& stack = CallStack[n];
    u8 mode = cpu.CPSR & 0x1F;
    u32 next = prev + (PrevThumb[n] ? 2 : 4);
    u32 lr = cpu.R[14] & ~1u;
    u32 sp = cpu.R[13];
    if (pc != next)
        JumpTarget[n] = pc;
    if (!stack.empty() && stack.back().Entry == pc)
        return;
    bool literalReturn = !PrevThumb[n] && (Prev2Instr[n] & 0x0F7FF000) == 0x051FE000 && IsIndirectJump(PrevInstr[n], false);
    if (pc != next && pc != lr && (lr == next || literalReturn))
    {
        PushFrame(n, Called(cpu, prev, pc, lr, sp, mode));
        return;
    }
    if (!stack.empty())
    {
        CallFrame& top = stack.back();
        if (top.SP == sp && top.Return == lr && top.Mode == mode && !top.Exception
            && ((pc != next && IsIndirectJump(PrevInstr[n], PrevThumb[n])) || KnownEntries[n].count(pc)))
        {
            top = Called(cpu, top.Site, pc, top.Return, top.SP, top.Mode);
            CallCounts[n][pc]++;
            if (Probes.Tagging())
                Probes.CountCall(n, pc);
            return;
        }
    }
    if (!IsPushLR(cpu.NextInstr[0], cpu.CPSR & 0x20))
        return;
    for (size_t k = 0; k < std::min<size_t>(stack.size(), 8); k++)
    {
        const CallFrame& f = stack[stack.size() - 1 - k];
        if (f.Return == lr || (f.Exception && f.Return + 4 == lr))
            return;
    }
    u32 entry = pc - JumpTarget[n] <= MaxPrologueOffset ? JumpTarget[n] : pc;
    CallFrame frame = Called(cpu, lr - ((cpu.R[14] & 1) ? 2 : 4), entry, lr, sp, mode);
    frame.HasArgs = entry == pc;
    PushFrame(n, frame);
}

static bool IsExceptionReturn(u32 instr, bool thumb)
{
    if (thumb)
        return false;
    if ((instr & 0x0C000000) == 0)
        return (instr & (1 << 20)) && ((instr >> 12) & 0xF) == 15;
    if ((instr & 0x0E000000) == 0x08000000)
        return (instr & (1 << 20)) && (instr & (1 << 22)) && (instr & (1 << 15));
    return false;
}

static bool IsStackAdjust(u32 instr, bool thumb)
{
    if (thumb)
    {
        u16 t = instr & 0xFFFF;
        return (t & 0xFF00) == 0xB000 || (t & 0xF600) == 0xB400;
    }
    u32 rn = (instr >> 16) & 0xF;
    u32 rd = (instr >> 12) & 0xF;
    if ((instr & 0x0C000000) == 0)
    {
        u32 op = (instr >> 21) & 0xF;
        return rd == 13 && rn == 13 && (op == 0x2 || op == 0x4);
    }
    if ((instr & 0x0E000000) == 0x08000000)
        return rn == 13 && (instr & (1 << 21)) && !(instr & (1 << 13));
    if ((instr & 0x0C000000) == 0x04000000)
        return rn == 13 && rd != 13 && (!(instr & (1 << 24)) || (instr & (1 << 21)));
    return false;
}

void Debugger::UnwindCalls(ARM& cpu, u32 pc)
{
    u8 n = cpu.Num;
    auto& stack = CallStack[n];
    u8 mode = cpu.CPSR & 0x1F;
    u32 sp = cpu.R[13];

    if (!Exceptions[n].empty() && IsExceptionMode(LastMode[n]) && IsExceptionReturn(PrevInstr[n], PrevThumb[n]))
    {
        Interrupted ex = std::move(Exceptions[n].back());
        Exceptions[n].pop_back();
        if ((ex.Return & ~1u) == pc || IsExceptionMode(mode))
            stack = std::move(ex.Stack);
        else
        {
            CallStack[n] = std::move(ex.Stack);
            SwitchStacks(n, ex.SP, sp);
            ResumeContext(cpu, pc, false);
            if (CallStack[n].empty())
            {
                CallStack[n] = UnwindThread(n, pc, sp, cpu.R[14], cpu.CPSR);
                SeedPending[n] = CallStack[n].empty();
            }
        }
        LastSP[n][(mode == 0x1F ? 0x10 : mode) & 0xF] = sp;
        LastMode[n] = mode;
    }

    u32& last = LastSP[n][(mode == 0x1F ? 0x10 : mode) & 0xF];
    if (last && sp != last && Exceptions[n].empty() && !IsExceptionMode(mode))
    {
        bool loaded = IsExceptionMode(LastMode[n]);
        bool unwound = sp > last && !stack.empty() && sp <= stack.front().SP;
        if (loaded || (!IsStackAdjust(PrevInstr[n], PrevThumb[n]) && !unwound))
            SwitchStacks(n, last, sp);
        if (loaded)
            ResumeContext(cpu, pc, true);
    }
    last = sp;
    LastMode[n] = mode;

    size_t depth = stack.size();
    for (size_t k = 0; k < std::min<size_t>(depth, 8); k++)
    {
        const CallFrame& f = stack[depth - 1 - k];
        if ((f.Return & ~1u) == pc && f.Mode == mode && cpu.R[13] >= f.SP)
        {
            stack.resize(depth - 1 - k);
            return;
        }
    }
    while (!stack.empty())
    {
        CallFrame& top = stack.back();
        if (top.Mode != mode || cpu.R[13] <= top.SP)
            return;
        stack.pop_back();
    }
}

void Debugger::OnInstr(ARM& cpu, u32 pc, bool redirected)
{
    u8 n = cpu.Num;
    u32 prev = CurPC[n];
    CurPC[n] = pc;

    if (ServiceRequested.load(std::memory_order_relaxed) && ServiceRequested.exchange(false))
    {
        if (ServiceHandler)
        {
            ContextGuard guard(*this, HookContext::Service, n);
            ServiceHandler();
        }
        Rearm();
    }

    if (CallTracking)
    {
        UnwindCalls(cpu, pc);
        TrackEntry(cpu, pc, prev);
    }
    // probes and breakpoint actions; an action that moves PC (return early, skip, set PC) means the instruction
    // at pc does not run: GdbCheckC calls the hooks again at the new PC
    if (Probes.InstrArmed(n) && Probes.OnInstr(cpu, pc))
        return;
    bool vector = IsExceptionMode(cpu.CPSR & 0x1F) && pc - cpu.ExceptionBase < 0x20;
    bool jumped = !vector && pc != prev + (PrevThumb[n] ? 2 : 4) && WritesPC(PrevInstr[n], PrevThumb[n]);
    Prev2Instr[n] = PrevInstr[n];
    PrevInstr[n] = cpu.NextInstr[0];
    PrevThumb[n] = cpu.CPSR & 0x20;

    if (Coverage)
    {
        u32 page = pc >> PageShift;
        if (!CoverageLastBits[n] || CoverageLastPage[n] != page)
        {
            CoverageLastBits[n] = &CoveragePages[n][page];
            CoverageLastPage[n] = page;
        }
        u32 idx = (pc & ((1u << PageShift) - 1)) >> 1;
        (*CoverageLastBits[n])[idx >> 6] |= 1ull << (idx & 63);
        if (Probes.Tagging())
            Probes.Cover(n, pc);
    }

    if ((TraceMask & (1 << n)) && pc >= TraceStart && pc <= TraceEnd)
    {
        u64 ts = n ? NDS.ARM7Timestamp : (NDS.ARM9Timestamp >> NDS.ARM9ClockShift);
        Trace[TraceHead] = {ts, pc, cpu.NextInstr[0], cpu.CPSR, n};
        if (++TraceHead == Trace.size())
        {
            TraceHead = 0;
            TraceWrapped = true;
        }
    }

    bool raise = false;
    BreakEvent ev {BreakKind::Step, n, 0, 0, pc, pc, 0};

    if (PendingValid[n])
    {
        PendingValid[n] = false;
        ev = Pending[n];
        raise = true;
    }
    if (BreakRequested[n].load(std::memory_order_relaxed) && BreakRequested[n].exchange(false) && !raise)
    {
        ev.Kind = BreakKind::Request;
        raise = true;
    }
    if (StepRemaining[n] && !redirected && --StepRemaining[n] == 0 && !raise)
    {
        ev.Kind = BreakKind::Step;
        raise = true;
    }
    if (StepModes[n] != StepMode::None && !raise && StepDone(cpu, pc, jumped))
    {
        StepModes[n] = StepMode::None;
        ev.Kind = BreakKind::Step;
        raise = true;
    }

    if (!ReturnedToHook(cpu, pc) && !ExecPages[n].empty() && ExecPages[n][pc >> PageShift])
    {
        ContextGuard guard(*this, HookContext::Hook, n);
        std::vector<int> hits;
        for (auto& b : Breakpoints)
            if (b.Enabled && (b.CPUMask & (1 << n)) && b.Addr == pc)
            {
                b.Hits++;
                hits.push_back(b.ID);
            }
        for (int id : hits)
        {
            auto it = std::find_if(Breakpoints.begin(), Breakpoints.end(), [id](const Breakpoint& b) { return b.ID == id; });
            if (it == Breakpoints.end())
                continue;
            BreakEvent hit {BreakKind::Exec, n, 0, id, pc, pc, 0};
            BreakCallback cb = it->Callback;
            if ((!cb || cb(hit)) && !raise)
            {
                ev = hit;
                raise = true;
            }
        }
    }

    if (raise)
    {
        ContextGuard guard(*this, HookContext::Hook, n);
        InstrBreakCPU = n;
        Raise(ev);
        InstrBreakCPU = -1;
    }
}

void Debugger::OnDMA(u8 n, u8 channel, u32 src, u32 addr, u8 size, u32 value)
{
    if (Probes.DataArmed(n))
        Probes.OnDMA(n, channel, src, addr, size, value);
    if (!LastWriter.empty() && (addr >> 24) == 0x02)
        LastWriter[(addr & 0x3FFFFF) >> 2] = DMAWriterTag | (channel << 1) | n;
    WatchHit(n, addr, size, value, true, channel);
}

void Debugger::OnData(ARM& cpu, u32 addr, u8 size, u32 value, bool write)
{
    u8 n = cpu.Num;
    if (write && !LastWriter.empty() && (addr >> 24) == 0x02)
        LastWriter[(addr & 0x3FFFFF) >> 2] = CurPC[n] | n;
    if (Probes.DataArmed(n))
    {
        // during an instruction R15 is 8 (ARM) or 4 (Thumb) ahead of it, hooks armed or not
        // (checked against CurPC over every data access of a DQ9 battle round: no difference)
        u32 pc = cpu.R[15] - ((cpu.CPSR & 0x20) ? 4 : 8);
        Probes.OnData(n, addr, size, value, write, pc);
    }
    WatchHit(n, addr, size, value, write, 0xFF);
}

void Debugger::WatchHit(u8 n, u32 addr, u8 size, u32 value, bool write, u8 dma)
{
    if (WatchPages[n].empty() || !WatchPages[n][addr >> PageShift])
        return;

    u8 kind = write ? Watch_Write : Watch_Read;
    u32 last = addr + size - 1;
    std::vector<int> hits;
    for (auto& w : Watchpoints)
        if (w.Enabled && (w.CPUMask & (1 << n)) && (w.Flags & kind) && addr <= w.End && last >= w.Start)
        {
            w.Hits++;
            hits.push_back(w.ID);
        }
    if (hits.empty())
        return;

    if (!write)
        value = Peek(n, addr, size);
    bool raiseDMA = false;
    BreakEvent dmaEvent {};
    for (int id : hits)
    {
        auto it = std::find_if(Watchpoints.begin(), Watchpoints.end(), [id](const Watchpoint& w) { return w.ID == id; });
        if (it == Watchpoints.end())
            continue;
        // A DMA write is not made by an instruction: its pc is the DMA tag, and it parks at once (the DMA
        // is between two units, the CPUs between two instructions) instead of at the CPU's next instruction.
        u32 pc = dma == 0xFF ? CurPC[n] : (DMAWriterTag | (dma << 1) | n);
        BreakEvent hit {write ? BreakKind::Write : BreakKind::Read, n, size, id, pc, addr, value, dma};
        BreakCallback cb = it->Callback;
        MidInstr[n]++;
        bool brk = !cb || cb(hit);
        MidInstr[n]--;
        if (!brk)
            continue;
        if (dma != 0xFF)
        {
            if (!raiseDMA)
            {
                dmaEvent = hit;
                raiseDMA = true;
            }
        }
        else if (!PendingValid[n])
        {
            Pending[n] = hit;
            PendingValid[n] = true;
            Armed[n] = true;
        }
    }
    if (raiseDMA)
    {
        // the service context: a write while parked here refetches a CPU pipeline it covers (SyncPipelines),
        // but no CPU is in its instruction hook, so memory.execute stays refused
        ContextGuard guard(*this, HookContext::Service, n);
        Raise(dmaEvent);
    }
}

void Debugger::OnCall(ARM& cpu)
{
    u8 n = cpu.Num;
    u32 entry = cpu.R[15] - ((cpu.CPSR & 0x20) ? 2 : 4);
    PushFrame(n, Called(cpu, CurPC[n], entry, cpu.R[14] & ~1u, cpu.R[13], (u8)(cpu.CPSR & 0x1F)));
}

void Debugger::OnException(ARM& cpu, u32 ret)
{
    u8 n = cpu.Num;
    u32 entry = cpu.R[15] - ((cpu.CPSR & 0x20) ? 2 : 4);
    if (Exceptions[n].size() >= 16)
        Exceptions[n].erase(Exceptions[n].begin());
    Exceptions[n].push_back({ret, ThreadSP(cpu), std::move(CallStack[n])});
    CallStack[n] = {{ret, entry, ret, cpu.R[13], (u8)(cpu.CPSR & 0x1F), true}};
}

void Debugger::OnFault(ARM& cpu, u32 pc, u32 addr)
{
    u8 n = cpu.Num;
    MidInstr[n]++;
    Raise({BreakKind::Fault, n, 0, 0, pc, addr, 0});
    MidInstr[n]--;
}

}
