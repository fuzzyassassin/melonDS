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

#include <stdio.h>

#include <QFileInfo>

#include "StartupOptions.h"
#include "DebugBridge.h"
#include "DebugSymbolsQt.h"
#include "DebugTools.h"
#include "EmuInstance.h"
#include "EmuThread.h"
#include "Platform.h"
#include "NDS.h"

using namespace melonDS;

namespace DebugTools
{

static void problem(EmuInstance* inst, int& count, const QString& text)
{
    QByteArray msg = text.toUtf8();
    Platform::Log(Platform::LogLevel::Error, "startup: %s\n", msg.constData());
    fflush(stdout);
    inst->osdAddMessage(0xFFA0A0, "%s", msg.constData());
    count++;
}

int applyStartup(EmuInstance* inst, const StartupOptions& opts, bool romBooted)
{
    int problems = 0;
    if (!opts.any())
        return 0;
    DebugBridge* bridge = inst->getDebugBridge();
    EmuThread* thread = inst->getEmuThread();
    DebugSymbols& symbols = bridge->symbols();

    bool needRom = !opts.Symbols.isEmpty() || !opts.Labels.isEmpty() || !opts.State.isEmpty();
    if (needRom && !romBooted)
        problem(inst, problems, "--symbols, --labels and --state need a ROM that boots; they were ignored");

    // Pause first, so nothing runs between the boot and the state with half of the breakpoints set.
    if (romBooted)
        thread->emuPause();

    if (romBooted && (!opts.Symbols.isEmpty() || !opts.Labels.isEmpty()))
    {
        // Through the remembered per-game paths, as the Symbols... and Labels... buttons do: autoLoadSymbols runs
        // again whenever the console restarts and would otherwise put the remembered ones back.
        if (!opts.Symbols.isEmpty())
        {
            QString error;
            if (LoadSymbols(symbols, opts.Symbols, error, inst->getNDS()))
                rememberSymbols(bridge, SymStr(symbols.source()));
            else
                problem(inst, problems, "cannot load symbols " + opts.Symbols + ": " + error);
        }
        if (!opts.Labels.isEmpty())
        {
            QString path = QFileInfo(opts.Labels).absoluteFilePath();
            std::string error;
            if (symbols.loadLabels(SymStr(path), error))
            {
                rememberLabels(bridge, path);
                if (symbols.labelsUnmatched())
                    problem(inst, problems, QString("%1 of %2 labels match no module in the loaded symbols")
                        .arg(symbols.labelsUnmatched()).arg(symbols.labelCount()));
            }
            else
                problem(inst, problems, "cannot load labels " + opts.Labels + ": " + SymStr(error));
        }
        emit bridge->symbolsChanged();
    }

    if (romBooted && !opts.State.isEmpty())
    {
        if (!QFileInfo(opts.State).isFile())
            problem(inst, problems, "no savestate " + opts.State);
        else if (!thread->loadState(opts.State))
            problem(inst, problems, "cannot load savestate " + opts.State);
        else
            inst->osdAddMessage(0, "State loaded from %s", QFileInfo(opts.State).fileName().toUtf8().constData());
    }

    if (!opts.Breaks.isEmpty())
    {
        if (inst->getGlobalConfig().GetBool("JIT.Enable"))
            problem(inst, problems, "JIT is on: breakpoints only work with the interpreter (Config > Emu settings)");
        std::vector<u32> addrs;
        for (const QString& text : opts.Breaks)
        {
            u32 addr;
            if (symbols.parseAddress(SymStr(text.trimmed()), addr))
                addrs.push_back(addr & ~1u);
            else
                problem(inst, problems, "cannot read the breakpoint address " + text);
        }
        bool ok = addrs.empty() || bridge->run([&addrs](NDS& nds) {
            for (u32 addr : addrs)
                nds.Debugger.AddBreakpoint(1, addr);
        });
        if (!ok)
            problem(inst, problems, "the console did not take the breakpoints (is a ROM running?)");
    }

    if (opts.Debugger)
        openByName("debugger", bridge, inst->getMainWindow());

    if (romBooted && !opts.Pause)
        thread->emuUnpause();
    return problems;
}

}
