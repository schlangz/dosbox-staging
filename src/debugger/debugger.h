// SPDX-FileCopyrightText:  2002-2025 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_DEBUG_H
#define DOSBOX_DEBUG_H

#include "config/config.h"
#include "config/setup.h"
#include "dosbox.h"
#include "hardware/memory.h"

#if C_DEBUGGER

void DEBUG_AddConfigSection(const ConfigPtr& conf);
void DEBUG_Init();
void DEBUG_Destroy();

void DEBUG_DrawScreen();
bool DEBUG_Breakpoint();
bool DEBUG_IntBreakpoint(uint8_t intNum);
void DEBUG_Enable(bool pressed);
void DEBUG_CheckExecuteBreakpoint(uint16_t seg, uint32_t off);
bool DEBUG_ExitLoop(void);
bool DEBUG_IsDebugging();

// True only once the CPU has actually come to rest inside DEBUG_Loop().
//
// DEBUG_IsDebugging() reports the *request*: DEBUG_Enable() sets it, but the
// loop handler it installs is only picked up once the currently running loop
// returns, so the guest keeps executing until then. Anything that reads or
// writes guest state out of band (the HTTP debugger API) must gate on this
// one, not on DEBUG_IsDebugging().
bool DEBUG_IsStopped();

void DEBUG_RefreshPage(int scroll);
Bitu DEBUG_EnableDebugger();

void LOG_StartUp();
void LOG_Init();
void LOG_Destroy();

extern Bitu cycle_count;
extern Bitu debugCallback;

#else

constexpr bool DEBUG_IsStopped()
{
	return false;
}

#endif // C_DEBUGGER

#if C_DEBUGGER && C_HEAVY_DEBUGGER
bool DEBUG_HeavyIsBreakpoint();
void DEBUG_HeavyWriteLogInstruction();

template <typename T>
void DEBUG_UpdateMemoryReadBreakpoints(const PhysPt addr);

// Suppresses memory-read breakpoint recording for the lifetime of the object.
//
// Memory breakpoints answer "did the *guest* touch this address". A read
// issued by an out-of-band consumer (the HTTP memory API) is not guest
// execution, so recording it produces a hit attributed to whichever
// instruction happens to run next. Scope one of these around such an access.
class DEBUG_OutOfBandMemoryAccess {
public:
	DEBUG_OutOfBandMemoryAccess();
	~DEBUG_OutOfBandMemoryAccess();

	DEBUG_OutOfBandMemoryAccess(const DEBUG_OutOfBandMemoryAccess&) = delete;
	DEBUG_OutOfBandMemoryAccess& operator=(const DEBUG_OutOfBandMemoryAccess&) = delete;
};

// Re-baselines every write-watching memory breakpoint (BPM and friends) whose
// watched byte falls in [start, start + len). Those breakpoints report a hit
// by noticing the byte differs from the value last seen, so an out-of-band
// write has to update that baseline or the next instruction is blamed for it.
void DEBUG_ResyncMemoryBreakpoints(const PhysPt start, const size_t len);

#else

template <typename T>
constexpr void DEBUG_UpdateMemoryReadBreakpoints(const PhysPt)
{
	// no-op
}

class DEBUG_OutOfBandMemoryAccess {
public:
	// Non-trivial so an unnamed guard object is not flagged as unused.
	DEBUG_OutOfBandMemoryAccess() {}
	~DEBUG_OutOfBandMemoryAccess() {}
};

constexpr void DEBUG_ResyncMemoryBreakpoints(const PhysPt, const size_t)
{
	// no-op
}
#endif // C_DEBUGGER && C_HEAVY_DEBUGGER

#endif // DOSBOX_DEBUG_H
