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

#ifndef STARTUPOPTIONS_H
#define STARTUPOPTIONS_H

#include <QString>
#include <QStringList>

class EmuInstance;

namespace DebugTools
{

// Debugging set up from the command line (--symbols, --labels, --state, --break, --pause, --debugger), applied
// once to the first instance after its ROM has booted.
struct StartupOptions
{
    QString Symbols;      // a dsd config.yaml (or its folder) or an "address name [size]" file; remembered per game
    QString Labels;       // an "address name [module]" file layered over them; remembered per game
    QString State;        // a savestate file to load
    QStringList Breaks;   // ARM9 execute breakpoints: an address, a symbol or label, or symbol+0x10
    bool Pause = false;   // leave the console paused afterwards
    bool Debugger = false; // open the debugger window

    bool any() const
    {
        return !Symbols.isEmpty() || !Labels.isEmpty() || !State.isEmpty() || !Breaks.isEmpty() || Pause || Debugger;
    }
};

// Applies the options; problems are logged (and shown on the OSD) without stopping the rest. romBooted says whether
// a ROM is running: the symbols, labels and state need one. Returns the number of problems.
int applyStartup(EmuInstance* inst, const StartupOptions& opts, bool romBooted);

}

#endif
