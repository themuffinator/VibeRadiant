/*
   Copyright (c) 2001, Loki software, inc.
   All rights reserved.

   Redistribution and use in source and binary forms, with or without modification,
   are permitted provided that the following conditions are met:

   Redistributions of source code must retain the above copyright notice, this list
   of conditions and the following disclaimer.

   Redistributions in binary form must reproduce the above copyright notice, this
   list of conditions and the following disclaimer in the documentation and/or
   other materials provided with the distribution.

   Neither the name of Loki software nor the names of its contributors may be used
   to endorse or promote products derived from this software without specific prior
   written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
   DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
   (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
   LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
   ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <vector>
#include "string/string.h"

void BuildMonitor_Construct();
void BuildMonitor_Destroy();

enum class BuildLaunchMode
{
	UsePreference = 0,
	ForceOff = 1,
	ForceOn = 2,
};

void BuildMonitor_Run( std::vector<CopiedString>& commands, const char* mapName, BuildLaunchMode launchMode = BuildLaunchMode::UsePreference );
void BuildMonitor_RunEngine( const char* mapName );
CopiedString Build_getEngineExecutable();

enum class BuildRuntimeState
{
	Idle = 0,
	Building = 1,
	Launching = 2,
	Succeeded = 3,
	Failed = 4,
};
BuildRuntimeState BuildMonitor_getRuntimeState();
const char* BuildMonitor_getRuntimeText();

extern bool g_WatchBSP_Enabled;
extern bool g_WatchBSP_LeakStop;
extern bool g_WatchBSP0_DumpLog;

inline constexpr const char* RADIANT_MONITOR_ADDRESS = "127.0.0.1:39000";
