// SPDX-FileCopyrightText:  2026-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Dynamic CALL recorder with conservative value provenance.
//
// Answers the question static analysis of PRCD.EXE keeps failing at: given
// sub_XXXXX(a1, a2, a3), what are the arguments really, and which object
// field did each one come from. It records every executed CALL with its
// actually-resolved runtime target (the point of the exercise for indirect
// and vtable dispatch), the register and stack state the caller prepared,
// and where those values were last read from and last written.
//
// Deliberately emits raw ground truth only. Callgraph construction,
// statistics and interpretation belong in an external tool reading the
// dump, not in here.
//
// core=normal only, by design. The hooks live in that interpreter's own
// dispatch and memory macros; recompiling cores are not instrumented and
// never will be. Enabling the recorder does not switch cores for you.

#ifndef DOSBOX_CALLREC_H
#define DOSBOX_CALLREC_H

#include "dosbox.h"

#include <cstdint>

#include "hardware/memory.h"

// How the CALL reached its target. Direct means the target was encoded in
// the instruction; indirect means it came from a register or memory, which
// is the case static analysis cannot resolve.
enum class CallrecKind : uint8_t {
	NearDirect   = 0,
	NearIndirect = 1,
	FarDirect    = 2,
	FarIndirect  = 3,
};

// Single gate for every hook below. Checked inline so a normal run pays one
// predictable branch per instruction and nothing else.
extern bool g_callrec_enabled;

void CALLREC_InstructionImpl(const PhysPt cseip);
void CALLREC_NotifyReadImpl(const PhysPt addr, const uint8_t size);
void CALLREC_NotifyWriteImpl(const PhysPt addr, const uint8_t size);
void CALLREC_CallImpl(const CallrecKind kind, const uint16_t from_cs,
                      const uint16_t from_ip, const uint16_t to_cs,
                      const uint16_t to_ip);

// Called from the core's dispatch loop with the linear address of the
// instruction about to run, after any LOADIP and before the opcode fetch.
static inline void CALLREC_Instruction(const PhysPt cseip)
{
	if (g_callrec_enabled) {
		CALLREC_InstructionImpl(cseip);
	}
}

// Called from the core's operand load/store macros, so the recorder sees
// the addresses the emulator itself resolved rather than re-deriving
// effective addresses and risking a different answer. Instruction fetches
// deliberately do not come through here.
static inline void CALLREC_NotifyRead(const PhysPt addr, const uint8_t size)
{
	if (g_callrec_enabled) {
		CALLREC_NotifyReadImpl(addr, size);
	}
}

static inline void CALLREC_NotifyWrite(const PhysPt addr, const uint8_t size)
{
	if (g_callrec_enabled) {
		CALLREC_NotifyWriteImpl(addr, size);
	}
}

// Called from each CALL path BEFORE the return address is pushed, so the
// captured stack is exactly what the caller prepared.
static inline void CALLREC_Call(const CallrecKind kind, const uint16_t from_cs,
                                const uint16_t from_ip, const uint16_t to_cs,
                                const uint16_t to_ip)
{
	if (g_callrec_enabled) {
		CALLREC_CallImpl(kind, from_cs, from_ip, to_cs, to_ip);
	}
}

// Control surface, used by the CALLREC console command and the HTTP routes.
void CALLREC_SetEnabled(const bool enabled);
bool CALLREC_IsEnabled();
void CALLREC_Reset();

// Writes CALLREC.JSONL and returns the number of call-edge records written,
// or -1 if the file could not be opened. Called at pause, on an explicit
// dump, and at shutdown.
int CALLREC_Dump();

// Counts for the status route: unique edges, unique call sites, total calls.
void CALLREC_GetStats(uint32_t& edges, uint32_t& call_sites, uint64_t& total_calls);

#endif // DOSBOX_CALLREC_H
