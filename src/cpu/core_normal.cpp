// SPDX-FileCopyrightText:  2024-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Needed for std::isnan in simde
#include <cmath>

#include "cpu/callback.h"
#include "cpu/cpu.h"
#include "cpu/mmx.h"
#include "cpu/paging.h"
#include "fpu/fpu.h"
#include "hardware/memory.h"
#include "hardware/pic.h"
#include "hardware/port.h"
#include "lazyflags.h"

#include "simde/x86/mmx.h"

#if C_DEBUGGER
#include "debugger/debugger.h"
#include "debugger/callrec.h"
#endif

#if (!C_CORE_INLINE)
#define RawLoadMb(off) mem_readb(off)
#define RawLoadMw(off) mem_readw(off)
#define RawLoadMd(off) mem_readd(off)
#define LoadMq(off) mem_readq(off)
#define RawSaveMb(off,val)	mem_writeb(off,val)
#define RawSaveMw(off,val)	mem_writew(off,val)
#define RawSaveMd(off,val)	mem_writed(off,val)
#define SaveMq(off,val) mem_writeq(off,val)
#else
#include "cpu/paging.h"
#define RawLoadMb(off) mem_readb_inline(off)
#define RawLoadMw(off) mem_readw_inline(off)
#define RawLoadMd(off) mem_readd_inline(off)
#define LoadMq(off) mem_readq_inline(off)
#define RawSaveMb(off,val)	mem_writeb_inline(off,val)
#define RawSaveMw(off,val)	mem_writew_inline(off,val)
#define RawSaveMd(off,val)	mem_writed_inline(off,val)
#define SaveMq(off,val) mem_writeq_inline(off,val)
#endif

#if C_DEBUGGER
// Operand accesses are routed through the CALL recorder so it sees the
// addresses this core itself resolved, instead of re-deriving effective
// addresses and risking a different answer than the one that really ran.
// Instruction fetches deliberately use the Raw* forms below, so the
// recorder never mistakes an opcode fetch for an operand read.
static inline uint8_t CallrecLoadMb(const PhysPt off)
{
	CALLREC_NotifyRead(off, 1);
	return RawLoadMb(off);
}
static inline uint16_t CallrecLoadMw(const PhysPt off)
{
	CALLREC_NotifyRead(off, 2);
	return RawLoadMw(off);
}
static inline uint32_t CallrecLoadMd(const PhysPt off)
{
	CALLREC_NotifyRead(off, 4);
	return RawLoadMd(off);
}
static inline void CallrecSaveMb(const PhysPt off, const uint8_t val)
{
	CALLREC_NotifyWrite(off, 1);
	RawSaveMb(off, val);
}
static inline void CallrecSaveMw(const PhysPt off, const uint16_t val)
{
	CALLREC_NotifyWrite(off, 2);
	RawSaveMw(off, val);
}
static inline void CallrecSaveMd(const PhysPt off, const uint32_t val)
{
	CALLREC_NotifyWrite(off, 4);
	RawSaveMd(off, val);
}
#define LoadMb(off) CallrecLoadMb(off)
#define LoadMw(off) CallrecLoadMw(off)
#define LoadMd(off) CallrecLoadMd(off)
#define SaveMb(off,val) CallrecSaveMb(off,val)
#define SaveMw(off,val) CallrecSaveMw(off,val)
#define SaveMd(off,val) CallrecSaveMd(off,val)
#else
#define LoadMb(off) RawLoadMb(off)
#define LoadMw(off) RawLoadMw(off)
#define LoadMd(off) RawLoadMd(off)
#define SaveMb(off,val) RawSaveMb(off,val)
#define SaveMw(off,val) RawSaveMw(off,val)
#define SaveMd(off,val) RawSaveMd(off,val)
#define CALLREC_Instruction(cseip) ((void)0)
#endif

// prefix_none.h is shared with the other cores, which do not have the
// recorder's per-instruction context. It defines this as a no-op unless
// something (only this core) has already defined it.
#if C_DEBUGGER
#define CALLREC_CORECALL(kind, tcs, tip) \
	CALLREC_Call(kind, SegValue(cs), callrec_insn_ip, tcs, tip)
#else
#define CALLREC_CORECALL(kind, tcs, tip) ((void)0)
#endif

extern Bitu cycle_count;

// Enable FPU escape instructions
#define CPU_FPU 1

#define CPU_PIC_CHECK 1
#define CPU_TRAP_CHECK 1

#define CPU_TRAP_DECODER	CPU_Core_Normal_Trap_Run

#define OPCODE_NONE			0x000
#define OPCODE_0F			0x100
#define OPCODE_SIZE			0x200

#define PREFIX_ADDR			0x1
#define PREFIX_REP			0x2

#define TEST_PREFIX_ADDR	(core.prefixes & PREFIX_ADDR)
#define TEST_PREFIX_REP		(core.prefixes & PREFIX_REP)

#define DO_PREFIX_SEG(_SEG)					\
	BaseDS=SegBase(_SEG);					\
	BaseSS=SegBase(_SEG);					\
	core.base_val_ds=_SEG;					\
	goto restart_opcode;

#define DO_PREFIX_ADDR()								\
	core.prefixes=(core.prefixes & ~PREFIX_ADDR) |		\
	(cpu.code.big ^ PREFIX_ADDR);						\
	core.ea_table=&EATable[(core.prefixes&1) * 256];	\
	goto restart_opcode;

#define DO_PREFIX_REP(_ZERO)				\
	core.prefixes|=PREFIX_REP;				\
	core.rep_zero=_ZERO;					\
	goto restart_opcode;

typedef PhysPt (*GetEAHandler)(void);

static const uint32_t AddrMaskTable[2]={0x0000ffff,0xffffffff};

static struct {
	Bitu opcode_index;
	PhysPt cseip;
	PhysPt base_ds,base_ss;
	SegNames base_val_ds;
	bool rep_zero;
	Bitu prefixes;
	GetEAHandler * ea_table;
} core;

#define GETIP		(core.cseip-SegBase(cs))
#define SAVEIP		reg_eip=GETIP;
#define LOADIP		core.cseip=(SegBase(cs)+reg_eip);

#define SegBase(c)	SegPhys(c)
#define BaseDS		core.base_ds
#define BaseSS		core.base_ss

static inline uint8_t Fetchb() {
	uint8_t temp=RawLoadMb(core.cseip);
	core.cseip+=1;
	return temp;
}

static inline uint16_t Fetchw() {
	uint16_t temp=RawLoadMw(core.cseip);
	core.cseip+=2;
	return temp;
}
static inline uint32_t Fetchd() {
	uint32_t temp=RawLoadMd(core.cseip);
	core.cseip+=4;
	return temp;
}

#define Push_16 CPU_Push16
#define Push_32 CPU_Push32
#define Pop_16 CPU_Pop16
#define Pop_32 CPU_Pop32

#include "instructions.h"
#include "core_normal/support.h"
#include "core_normal/string.h"


#define EALookupTable (core.ea_table)

Bits CPU_Core_Normal_Run() noexcept
{
	while (CPU_Cycles-->0) {
		LOADIP;
		// IP of the instruction about to run, captured before anything
		// advances reg_eip, so a CALL can report its own call site
		// rather than its return address. Prefix bytes jump back to
		// restart_opcode without re-running LOADIP, so this stays
		// pointing at the real start of the instruction.
		const uint16_t callrec_insn_ip = (uint16_t)reg_eip;
		(void)callrec_insn_ip;
		core.opcode_index=cpu.code.big*0x200;
		core.prefixes=cpu.code.big;
		core.ea_table=&EATable[cpu.code.big*256];
		BaseDS=SegBase(ds);
		BaseSS=SegBase(ss);
		core.base_val_ds=ds;
#if C_DEBUGGER
		CALLREC_Instruction(core.cseip);
#if C_HEAVY_DEBUGGER
		if (DEBUG_HeavyIsBreakpoint()) {
			FillFlags();
			return debugCallback;
		};
#endif
		cycle_count++;
#endif
restart_opcode:
		switch (core.opcode_index+Fetchb()) {
		#include "core_normal/prefix_none.h"
		#include "core_normal/prefix_0f.h"
		#include "core_normal/prefix_66.h"
		#include "core_normal/prefix_66_0f.h"
		default:
		illegal_opcode:
#if C_DEBUGGER
			{
				Bitu len=(GETIP-reg_eip);
				LOADIP;
				if (len>16) len=16;
				char tempcode[16*2+1];char * writecode=tempcode;
				for (;len>0;len--) {
					sprintf(writecode,"%02X",mem_readb(core.cseip++));
					writecode+=2;
				}
				LOG(LOG_CPU,LOG_NORMAL)("Illegal/Unhandled opcode %s",tempcode);
			}
#endif
			CPU_Exception(6,0);
			continue;
		}
		SAVEIP;
	}
	FillFlags();
	return CBRET_NONE;
decode_end:
	SAVEIP;
	FillFlags();
	return CBRET_NONE;
}

Bits CPU_Core_Normal_Trap_Run() noexcept
{
	Bits oldCycles = CPU_Cycles;
	CPU_Cycles = 1;
	cpu.trap_skip = false;

	Bits ret=CPU_Core_Normal_Run();
	if (!cpu.trap_skip) CPU_DebugException(DBINT_STEP,reg_eip);
	CPU_Cycles = oldCycles-1;
	cpudecoder = &CPU_Core_Normal_Run;

	return ret;
}

void CPU_Core_Normal_Init(void) {

}

