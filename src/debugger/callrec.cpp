// SPDX-FileCopyrightText:  2026-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "debugger/callrec.h"

#if C_DEBUGGER

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpu/cpu.h"
#include "cpu/paging.h"
#include "cpu/registers.h"
#include "hardware/memory.h"
#include "hardware/pic.h"
#include "misc/std_filesystem.h"

// ***************************************************************************
// Tunables
// ***************************************************************************

// Stack words captured at SS:SP for every recorded call.
constexpr int CallrecStackWords = 32;

// Instructions kept in the rolling history, and how many of them are
// attached to a call record when one is first seen.
constexpr int CallrecRingEntries    = 128;
constexpr int CallrecRingAttached   = 64;
constexpr int CallrecRingCodeBytes  = 8;

// Stack slots reported with full provenance for each call.
constexpr int CallrecStackArgs = 8;

// Conventional memory, the range the shadow covers.
constexpr uint32_t CallrecShadowBytes = 1024 * 1024;

// ***************************************************************************
// Provenance model
// ***************************************************************************
//
// Safety rule, applied everywhere below: an operation whose provenance is
// not explicitly modelled must INVALIDATE what it touches, never leave a
// stale answer in place and never invent one. A wrong provenance chain
// reads exactly as plausible as a right one, so the only acceptable
// failure mode is "unknown".
//
// V1 models exactly: register-to-register MOV (propagates the source's
// existing origin unchanged), 16-bit register loads from memory, PUSH and
// POP, and 16-bit immediate loads. Everything else invalidates.

enum class ProvStatus : uint8_t { Unknown = 0, Exact = 1, Mixed = 2 };

enum class OriginKind : uint8_t {
	None = 0,
	RegisterCopy,
	MemoryRead,
	StackLoad,
	Immediate,
};

static const char* status_name(const ProvStatus s)
{
	switch (s) {
	case ProvStatus::Exact: return "exact";
	case ProvStatus::Mixed: return "mixed";
	default: return "unknown";
	}
}

static const char* origin_name(const OriginKind k)
{
	switch (k) {
	case OriginKind::RegisterCopy: return "register_copy";
	case OriginKind::MemoryRead: return "memory_read";
	case OriginKind::StackLoad: return "stack_load";
	case OriginKind::Immediate: return "immediate";
	default: return "none";
	}
}

struct Provenance {
	uint32_t last_writer_addr = 0; // instruction that last wrote the value
	uint32_t origin_addr      = 0; // instruction the value ultimately came from
	uint32_t mem_addr         = 0; // resolved address, for the memory kinds
	uint8_t mem_size          = 0;
	OriginKind origin_kind    = OriginKind::None;
	ProvStatus status         = ProvStatus::Unknown;
	// Set when only one byte of the 16-bit register was defined by the
	// origin, so nobody reads the full register value as if all of it
	// came from there.
	bool partial   = false;
	bool high_byte = false;
};

// x86 encoding order, so a ModRM reg/rm field indexes this directly.
enum RegIndex : uint8_t { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI };
constexpr int NumGpRegs = 8;

static const char* const RegNames[NumGpRegs] =
        {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"};

static Provenance reg_prov[NumGpRegs] = {};

static uint16_t read_gp(const int idx)
{
	switch (idx) {
	case R_AX: return reg_ax;
	case R_CX: return reg_cx;
	case R_DX: return reg_dx;
	case R_BX: return reg_bx;
	case R_SP: return reg_sp;
	case R_BP: return reg_bp;
	case R_SI: return reg_si;
	default: return reg_di;
	}
}

// ***************************************************************************
// Byte-granular memory shadow
// ***************************************************************************
//
// Every write allocates an event; every byte it covers points at that
// event. Byte granularity is the point: a word write followed by a byte
// write overlapping it must not let one entry tell a wrong story about the
// byte that was overwritten later. A read spanning bytes from different
// events is 'mixed' -- known exactly, and known not to be a single origin.

struct WriteEvent {
	uint32_t insn_addr   = 0; // instruction that performed the write
	uint32_t addr        = 0;
	uint32_t origin_addr = 0; // where the stored value came from
	uint32_t origin_mem  = 0;
	uint8_t size         = 0;
	OriginKind origin_kind = OriginKind::None;
	ProvStatus origin_status = ProvStatus::Unknown;
};

static std::vector<uint32_t> shadow;    // byte -> write event id, 0 = never
static std::vector<WriteEvent> events;  // index 0 is the "never written" sentinel

static void shadow_init()
{
	shadow.assign(CallrecShadowBytes, 0);
	events.clear();
	events.push_back(WriteEvent{});
}

// Resolves the provenance of a memory range from the shadow.
static void shadow_query(const uint32_t addr, const uint8_t size,
                         ProvStatus& status, uint32_t& last_write,
                         const WriteEvent** event_out)
{
	status     = ProvStatus::Unknown;
	last_write = 0;
	if (event_out) {
		*event_out = nullptr;
	}
	if (addr >= CallrecShadowBytes || size == 0) {
		return;
	}

	const uint32_t first = shadow[addr];
	if (first == 0) {
		return;
	}

	bool all_same = true;
	for (uint8_t i = 1; i < size; ++i) {
		const uint32_t a = addr + i;
		if (a >= CallrecShadowBytes || shadow[a] != first) {
			all_same = false;
			break;
		}
	}

	const WriteEvent& ev = events[first];
	last_write           = ev.insn_addr;
	status               = all_same ? ProvStatus::Exact : ProvStatus::Mixed;
	if (event_out && all_same) {
		*event_out = &ev;
	}
}

// ***************************************************************************
// Instruction classification
// ***************************************************************************

enum class InsnClass : uint8_t {
	Other = 0,   // not modelled; invalidate per the write mask
	MovRegReg,   // dst <- src, pure copy
	MovRegMem,   // dst <- [mem]
	MovRegMem8,  // dst's low or high byte <- [mem], 8-bit load
	MovMemReg,   // [mem] <- src
	MovRegImm,   // dst <- immediate
	PushReg,     // [stack] <- src
	PushMem,     // [stack] <- [mem]
	PushImm,     // [stack] <- immediate
	PopReg,      // dst <- [stack]
	LoadFarPtr,  // LDS/LES: dst <- [mem], plus a segment register
};

struct InsnInfo {
	InsnClass cls   = InsnClass::Other;
	uint8_t dst     = 0xFF; // GP register index written, 0xFF for none
	uint8_t src     = 0xFF; // GP register index read, 0xFF for none
	uint8_t wr_mask = 0xFF; // GP registers this may write, for the Other case
	bool high_byte  = false; // MovRegMem8 wrote the high half
};

static uint8_t peek(const PhysPt addr, const int n)
{
	uint8_t b = 0;
	if (mem_readb_checked(addr + static_cast<PhysPt>(n), &b)) {
		return 0;
	}
	return b;
}

// Bit position of a GP register in a write mask.
static constexpr uint8_t mask_of(const uint8_t reg)
{
	return static_cast<uint8_t>(1u << reg);
}

// Which GP registers an unmodelled opcode may write. 0xFF means "no idea",
// which invalidates everything -- the safe answer, never the silent one.
static uint8_t write_mask_for(const uint8_t op, const uint8_t modrm)
{
	const uint8_t mod = static_cast<uint8_t>(modrm >> 6);
	const uint8_t reg = static_cast<uint8_t>((modrm >> 3) & 7);
	const uint8_t rm  = static_cast<uint8_t>(modrm & 7);

	const auto rm_mask = [&]() -> uint8_t {
		return (mod == 3) ? mask_of(rm) : 0;
	};

	switch (op) {
	// ALU groups: 00/01 and 08/09 style write r/m, 02/03 style write reg,
	// 04/05 style write AL/AX.
	case 0x00: case 0x01: case 0x08: case 0x09:
	case 0x10: case 0x11: case 0x18: case 0x19:
	case 0x20: case 0x21: case 0x28: case 0x29:
	case 0x30: case 0x31:
		return rm_mask();
	case 0x02: case 0x03: case 0x0A: case 0x0B:
	case 0x12: case 0x13: case 0x1A: case 0x1B:
	case 0x22: case 0x23: case 0x2A: case 0x2B:
	case 0x32: case 0x33:
		return mask_of(reg);
	case 0x04: case 0x05: case 0x0C: case 0x0D:
	case 0x14: case 0x15: case 0x1C: case 0x1D:
	case 0x24: case 0x25: case 0x2C: case 0x2D:
	case 0x34: case 0x35:
		return mask_of(R_AX);

	// CMP and TEST write no register.
	case 0x38: case 0x39: case 0x3A: case 0x3B:
	case 0x3C: case 0x3D:
	case 0x84: case 0x85: case 0xA8: case 0xA9:
		return 0;

	case 0x40: case 0x41: case 0x42: case 0x43: // INC r16
	case 0x44: case 0x45: case 0x46: case 0x47:
		return mask_of(static_cast<uint8_t>(op - 0x40));
	case 0x48: case 0x49: case 0x4A: case 0x4B: // DEC r16
	case 0x4C: case 0x4D: case 0x4E: case 0x4F:
		return mask_of(static_cast<uint8_t>(op - 0x48));

	case 0x60: return mask_of(R_SP);           // PUSHA
	case 0x61: return 0xFF;                    // POPA writes everything
	case 0x69: case 0x6B: return mask_of(reg); // IMUL r16, r/m16, imm

	case 0x70: case 0x71: case 0x72: case 0x73: // Jcc
	case 0x74: case 0x75: case 0x76: case 0x77:
	case 0x78: case 0x79: case 0x7A: case 0x7B:
	case 0x7C: case 0x7D: case 0x7E: case 0x7F:
		return 0;

	case 0x80: case 0x81: case 0x82: case 0x83: // ALU r/m, imm
		return (reg == 7) ? 0 : rm_mask();     // /7 is CMP

	case 0x86: case 0x87: // XCHG: both operands change
		return static_cast<uint8_t>(mask_of(reg) | rm_mask());

	case 0x8C: return rm_mask();               // MOV r/m16, sreg
	case 0x8D: return mask_of(reg);            // LEA
	case 0x8E: return 0;                       // MOV sreg, r/m16

	case 0x90: return 0;                       // NOP
	case 0x91: case 0x92: case 0x93:           // XCHG AX, r16
	case 0x94: case 0x95: case 0x96: case 0x97:
		return static_cast<uint8_t>(mask_of(R_AX) |
		                            mask_of(static_cast<uint8_t>(op - 0x90)));
	case 0x98: return mask_of(R_AX);           // CBW
	case 0x99: return mask_of(R_DX);           // CWD
	case 0x9C: case 0x9D: case 0x9E: return 0; // PUSHF/POPF/SAHF
	case 0x9F: return mask_of(R_AX);           // LAHF

	case 0xA4: case 0xA5: return static_cast<uint8_t>(mask_of(R_SI) | mask_of(R_DI)); // MOVS
	case 0xA6: case 0xA7: return static_cast<uint8_t>(mask_of(R_SI) | mask_of(R_DI)); // CMPS
	case 0xAA: case 0xAB: return mask_of(R_DI);                                        // STOS
	case 0xAC: case 0xAD: return static_cast<uint8_t>(mask_of(R_AX) | mask_of(R_SI));   // LODS
	case 0xAE: case 0xAF: return mask_of(R_DI);                                        // SCAS

	case 0xC0: case 0xC1: return rm_mask();    // shift r/m, imm8
	case 0xC2: case 0xC3: return 0;            // RET
	case 0xC6: case 0xC7: return rm_mask();    // MOV r/m, imm
	case 0xC9: return static_cast<uint8_t>(mask_of(R_SP) | mask_of(R_BP)); // LEAVE
	case 0xCA: case 0xCB: case 0xCF: return 0; // RETF / IRET

	case 0xD0: case 0xD1: case 0xD2: case 0xD3: return rm_mask(); // shifts
	case 0xD7: return mask_of(R_AX);           // XLAT

	case 0xE4: case 0xE5: return mask_of(R_AX); // IN acc, imm8
	case 0xE6: case 0xE7: return 0;             // OUT
	case 0xEC: case 0xED: return mask_of(R_AX); // IN acc, DX
	case 0xEE: case 0xEF: return 0;             // OUT

	case 0xE8: case 0xE9: case 0xEA: case 0xEB: return 0; // CALL/JMP

	case 0xF5: case 0xF8: case 0xF9: case 0xFA:
	case 0xFB: case 0xFC: case 0xFD: return 0;  // flag ops

	case 0xF6: case 0xF7:
		switch (reg) {
		case 0: case 1: return 0;              // TEST
		case 2: case 3: return rm_mask();      // NOT / NEG
		default: return static_cast<uint8_t>(mask_of(R_AX) | mask_of(R_DX));
		}

	case 0xFE: return rm_mask();               // INC/DEC r/m8
	case 0xFF:
		switch (reg) {
		case 0: case 1: return rm_mask();      // INC/DEC r/m16
		case 2: case 3: case 4: case 5: return 0; // CALL / JMP
		case 6: return 0;                      // PUSH r/m16
		default: return 0xFF;
		}

	default: return 0xFF;
	}
}

// Classifies the instruction at cseip. Any prefix at all makes the
// instruction unmodelled: operand and address size prefixes change the
// semantics this classifier assumes, and a segment override changes which
// address a load resolves to. Being wrong here is exactly the failure mode
// this whole design exists to avoid, so it declines to guess.
static InsnInfo classify(const PhysPt cseip)
{
	InsnInfo info = {};

	const uint8_t op = peek(cseip, 0);
	switch (op) {
	case 0x26: case 0x2E: case 0x36: case 0x3E: // segment overrides
	case 0x64: case 0x65:
	case 0x66: case 0x67:                       // operand / address size
	case 0xF0: case 0xF2: case 0xF3:            // LOCK / REP
		info.cls     = InsnClass::Other;
		info.wr_mask = 0xFF;
		return info;
	default: break;
	}

	const uint8_t modrm = peek(cseip, 1);
	const uint8_t mod   = static_cast<uint8_t>(modrm >> 6);
	const uint8_t reg   = static_cast<uint8_t>((modrm >> 3) & 7);
	const uint8_t rm    = static_cast<uint8_t>(modrm & 7);

	switch (op) {
	case 0x89: // MOV r/m16, r16
		if (mod == 3) {
			info.cls = InsnClass::MovRegReg;
			info.dst = rm;
			info.src = reg;
		} else {
			info.cls = InsnClass::MovMemReg;
			info.src = reg;
		}
		return info;

	case 0x8B: // MOV r16, r/m16
		if (mod == 3) {
			info.cls = InsnClass::MovRegReg;
			info.dst = reg;
			info.src = rm;
		} else {
			info.cls = InsnClass::MovRegMem;
			info.dst = reg;
		}
		return info;

	case 0x8A: // MOV r8, r/m8
		// A load into a sub-register is still a load, and this is the
		// exact shape PRCD.EXE uses to read its byte-sized struct
		// fields ("mov al,[si+4]"), so declining to model it would
		// throw away the answer this tool exists to give. What is NOT
		// claimed is that the whole 16-bit register came from there:
		// the record carries the byte size and which half was written,
		// and the other half keeps whatever it had.
		if (mod != 3) {
			info.cls       = InsnClass::MovRegMem8;
			info.dst       = static_cast<uint8_t>(reg & 3);
			info.high_byte = (reg >= 4);
			return info;
		}
		break;

	case 0xA1: // MOV AX, moffs16
		info.cls = InsnClass::MovRegMem;
		info.dst = R_AX;
		return info;

	case 0xA3: // MOV moffs16, AX
		info.cls = InsnClass::MovMemReg;
		info.src = R_AX;
		return info;

	case 0xB8: case 0xB9: case 0xBA: case 0xBB: // MOV r16, imm16
	case 0xBC: case 0xBD: case 0xBE: case 0xBF:
		info.cls = InsnClass::MovRegImm;
		info.dst = static_cast<uint8_t>(op - 0xB8);
		return info;

	case 0x50: case 0x51: case 0x52: case 0x53: // PUSH r16
	case 0x54: case 0x55: case 0x56: case 0x57:
		info.cls = InsnClass::PushReg;
		info.src = static_cast<uint8_t>(op - 0x50);
		return info;

	case 0x58: case 0x59: case 0x5A: case 0x5B: // POP r16
	case 0x5C: case 0x5D: case 0x5E: case 0x5F:
		info.cls = InsnClass::PopReg;
		info.dst = static_cast<uint8_t>(op - 0x58);
		return info;

	case 0x68: case 0x6A: // PUSH imm16 / imm8
		info.cls = InsnClass::PushImm;
		return info;

	case 0xC4: case 0xC5: // LES / LDS
		if (mod != 3) {
			info.cls = InsnClass::LoadFarPtr;
			info.dst = reg;
			return info;
		}
		break;

	case 0x8F: // POP r/m16
		if (mod == 3) {
			info.cls = InsnClass::PopReg;
			info.dst = rm;
			return info;
		}
		break;

	case 0xFF:
		if (reg == 6) { // PUSH r/m16
			if (mod == 3) {
				info.cls = InsnClass::PushReg;
				info.src = rm;
			} else {
				info.cls = InsnClass::PushMem;
			}
			return info;
		}
		break;

	default: break;
	}

	info.cls     = InsnClass::Other;
	info.wr_mask = write_mask_for(op, modrm);
	return info;
}

// ***************************************************************************
// Rolling instruction history
// ***************************************************************************

struct RingEntry {
	uint32_t addr = 0;
	// The addresses this instruction actually touched, taken from the
	// emulator's own resolved accesses rather than re-derived here. This
	// is what makes the history answer "where did this value come from"
	// on sight, without trusting the provenance engine.
	uint32_t read_addr  = 0;
	uint32_t write_addr = 0;
	uint8_t read_size   = 0;
	uint8_t write_size  = 0;
	uint16_t regs[NumGpRegs] = {};
	uint8_t code[CallrecRingCodeBytes] = {};
};

static RingEntry ring[CallrecRingEntries] = {};
static uint32_t ring_pos                  = 0;
static uint64_t ring_total                = 0;
static RingEntry* cur_ring                = nullptr;

// ***************************************************************************
// Recorded calls
// ***************************************************************************

struct ArgReport {
	uint16_t value = 0;
	Provenance call_prov = {};
	// Memory level, filled only when the call provenance bottoms out at a
	// memory access.
	bool has_mem            = false;
	uint32_t mem_addr       = 0;
	ProvStatus mem_status   = ProvStatus::Unknown;
	uint32_t mem_last_write = 0;
};

struct CallRecord {
	uint64_t count = 0;
	// Which execution the stored argument/history sample came from.
	uint64_t sample_hits = 0;
	CallrecKind kind = CallrecKind::NearDirect;
	uint16_t from_cs = 0, from_ip = 0, to_cs = 0, to_ip = 0;
	uint16_t regs[NumGpRegs] = {};
	uint16_t ds = 0, es = 0, ss = 0, fs = 0, gs = 0;
	uint16_t stack[CallrecStackWords] = {};
	ArgReport reg_args[NumGpRegs] = {};
	ArgReport stack_args[CallrecStackArgs] = {};
	RingEntry ring_snapshot[CallrecRingAttached] = {};
	uint32_t ring_snapshot_len = 0;
};

struct EdgeKey {
	uint32_t from = 0;
	uint32_t to   = 0;
	uint8_t kind  = 0;

	bool operator==(const EdgeKey& o) const
	{
		return from == o.from && to == o.to && kind == o.kind;
	}
};

struct EdgeKeyHash {
	size_t operator()(const EdgeKey& k) const
	{
		return (static_cast<size_t>(k.from) * 0x9E3779B1u) ^
		       (static_cast<size_t>(k.to) * 0x85EBCA77u) ^ k.kind;
	}
};

static std::unordered_map<EdgeKey, CallRecord, EdgeKeyHash> edges;
static uint64_t total_calls = 0;

// ***************************************************************************
// Live recorder state
// ***************************************************************************

bool g_callrec_enabled = false;

static uint32_t cur_insn_addr  = 0;
static InsnInfo cur_insn       = {};
static uint16_t cur_regs[NumGpRegs] = {};
static bool have_prev          = false;

static uint32_t prev_insn_addr = 0;
static InsnInfo prev_insn      = {};
static uint16_t prev_regs[NumGpRegs] = {};

// First operand read performed by the instruction currently executing.
static uint32_t cur_read_addr = 0;
static uint8_t cur_read_size  = 0;
static bool cur_read_valid    = false;
static uint32_t prev_read_addr = 0;
static uint8_t prev_read_size  = 0;
static bool prev_read_valid    = false;

static void invalidate_reg(const int idx)
{
	reg_prov[idx] = Provenance{};
}

static void invalidate_all_regs()
{
	for (int i = 0; i < NumGpRegs; ++i) {
		invalidate_reg(i);
	}
}

// Applies the provenance effect of the instruction that has just finished.
static void finalize_previous()
{
	if (!have_prev) {
		return;
	}

	const InsnInfo& in = prev_insn;
	uint8_t handled    = 0; // registers whose provenance was set deliberately

	switch (in.cls) {
	case InsnClass::MovRegReg:
		if (in.dst < NumGpRegs && in.src < NumGpRegs) {
			// A pure copy carries the source's origin forward. It
			// does NOT become "originated here" -- that would lose
			// the real origin, which is the whole answer wanted.
			Provenance p       = reg_prov[in.src];
			p.last_writer_addr = prev_insn_addr;
			reg_prov[in.dst]   = p;
			handled            = mask_of(in.dst);
		}
		break;

	case InsnClass::MovRegMem:
	case InsnClass::MovRegMem8:
	case InsnClass::LoadFarPtr:
		if (in.dst < NumGpRegs) {
			Provenance p = {};
			if (prev_read_valid) {
				p.last_writer_addr = prev_insn_addr;
				p.origin_addr      = prev_insn_addr;
				p.origin_kind      = OriginKind::MemoryRead;
				p.mem_addr         = prev_read_addr;
				p.mem_size         = prev_read_size;
				p.status           = ProvStatus::Exact;
				p.partial   = (in.cls == InsnClass::MovRegMem8);
				p.high_byte = in.high_byte;
			}
			reg_prov[in.dst] = p;
			handled          = mask_of(in.dst);
		}
		break;

	case InsnClass::PopReg:
		if (in.dst < NumGpRegs) {
			Provenance p = {};
			if (prev_read_valid) {
				p.last_writer_addr = prev_insn_addr;
				p.origin_addr      = prev_insn_addr;
				p.origin_kind      = OriginKind::StackLoad;
				p.mem_addr         = prev_read_addr;
				p.mem_size         = prev_read_size;
				p.status           = ProvStatus::Exact;
			}
			reg_prov[in.dst] = p;
			handled          = mask_of(in.dst);
		}
		break;

	case InsnClass::MovRegImm:
		if (in.dst < NumGpRegs) {
			Provenance p       = {};
			p.last_writer_addr = prev_insn_addr;
			p.origin_addr      = prev_insn_addr;
			p.origin_kind      = OriginKind::Immediate;
			p.status           = ProvStatus::Exact;
			reg_prov[in.dst]   = p;
			handled            = mask_of(in.dst);
		}
		break;

	case InsnClass::MovMemReg:
	case InsnClass::PushReg:
	case InsnClass::PushMem:
	case InsnClass::PushImm:
		// Stores write memory, not a GP register. The shadow entry was
		// already created by the write notification.
		break;

	case InsnClass::Other:
	default:
		if (in.wr_mask == 0xFF) {
			invalidate_all_regs();
		} else {
			for (int i = 0; i < NumGpRegs; ++i) {
				if (in.wr_mask & mask_of(static_cast<uint8_t>(i))) {
					invalidate_reg(i);
				}
			}
		}
		break;
	}

	// Backstop: anything whose value moved without this having been a
	// deliberate, modelled write is invalidated regardless of what the
	// table above believed. Cheap, and strictly safer than trusting the
	// classifier alone.
	for (int i = 0; i < NumGpRegs; ++i) {
		if (handled & mask_of(static_cast<uint8_t>(i))) {
			continue;
		}
		if (read_gp(i) != prev_regs[i]) {
			invalidate_reg(i);
		}
	}

	// SP moves with every stack operation and never carries a meaningful
	// origin, so it is never claimed as known.
	invalidate_reg(R_SP);
}

void CALLREC_InstructionImpl(const PhysPt cseip)
{
	finalize_previous();

	prev_insn_addr = cur_insn_addr;
	prev_insn      = cur_insn;
	memcpy(prev_regs, cur_regs, sizeof(prev_regs));
	prev_read_addr  = cur_read_addr;
	prev_read_size  = cur_read_size;
	prev_read_valid = cur_read_valid;

	cur_insn_addr  = cseip;
	cur_insn       = classify(cseip);
	cur_read_valid = false;
	cur_read_addr  = 0;
	cur_read_size  = 0;
	for (int i = 0; i < NumGpRegs; ++i) {
		cur_regs[i] = read_gp(i);
	}
	have_prev = true;

	RingEntry& e = ring[ring_pos];
	e            = RingEntry{};
	e.addr       = cseip;
	memcpy(e.regs, cur_regs, sizeof(e.regs));
	for (int i = 0; i < CallrecRingCodeBytes; ++i) {
		e.code[i] = peek(cseip, i);
	}
	cur_ring = &e;
	ring_pos = (ring_pos + 1) % CallrecRingEntries;
	++ring_total;
}

void CALLREC_NotifyReadImpl(const PhysPt addr, const uint8_t size)
{
	if (!cur_read_valid) {
		cur_read_addr  = addr;
		cur_read_size  = size;
		cur_read_valid = true;
		if (cur_ring) {
			cur_ring->read_addr = addr;
			cur_ring->read_size = size;
		}
	}
}

void CALLREC_NotifyWriteImpl(const PhysPt addr, const uint8_t size)
{
	if (cur_ring && cur_ring->write_size == 0) {
		cur_ring->write_addr = addr;
		cur_ring->write_size = size;
	}

	if (addr >= CallrecShadowBytes) {
		return;
	}

	WriteEvent ev = {};
	ev.insn_addr  = cur_insn_addr;
	ev.addr       = addr;
	ev.size       = size;

	// Carry the stored value's own provenance into the event where the
	// instruction's semantics are modelled. Where they are not, the event
	// still records which instruction wrote here -- that part is always
	// known -- but claims nothing about where the value came from.
	switch (cur_insn.cls) {
	case InsnClass::MovMemReg:
	case InsnClass::PushReg:
		if (cur_insn.src < NumGpRegs) {
			const Provenance& p = reg_prov[cur_insn.src];
			ev.origin_kind      = p.origin_kind;
			ev.origin_addr      = p.origin_addr;
			ev.origin_mem       = p.mem_addr;
			ev.origin_status    = p.status;
		}
		break;
	case InsnClass::PushImm:
		ev.origin_kind   = OriginKind::Immediate;
		ev.origin_addr   = cur_insn_addr;
		ev.origin_status = ProvStatus::Exact;
		break;
	case InsnClass::PushMem:
		ev.origin_kind   = OriginKind::MemoryRead;
		ev.origin_addr   = cur_insn_addr;
		ev.origin_mem    = cur_read_valid ? cur_read_addr : 0;
		ev.origin_status = cur_read_valid ? ProvStatus::Exact
		                                  : ProvStatus::Unknown;
		break;
	default:
		break;
	}

	events.push_back(ev);
	const auto id = static_cast<uint32_t>(events.size() - 1);

	const uint32_t end = std::min(addr + static_cast<uint32_t>(size),
	                              CallrecShadowBytes);
	for (uint32_t a = addr; a < end; ++a) {
		shadow[a] = id;
	}
}

// Fills in the three-level report for one value.
static ArgReport report_for(const uint16_t value, const Provenance& p)
{
	ArgReport r  = {};
	r.value      = value;
	r.call_prov  = p;

	if (p.status == ProvStatus::Exact &&
	    (p.origin_kind == OriginKind::MemoryRead ||
	     p.origin_kind == OriginKind::StackLoad)) {
		r.has_mem  = true;
		r.mem_addr = p.mem_addr;
		shadow_query(p.mem_addr,
		             p.mem_size ? p.mem_size : 2,
		             r.mem_status,
		             r.mem_last_write,
		             nullptr);
	}
	return r;
}

void CALLREC_CallImpl(const CallrecKind kind, const uint16_t from_cs,
                      const uint16_t from_ip, const uint16_t to_cs,
                      const uint16_t to_ip)
{
	// The instruction currently executing is the CALL itself, and its
	// effects on registers have not happened yet, so provenance is exactly
	// what the caller prepared. Flush the previous instruction's effect
	// first so a value set immediately before the CALL is already visible.
	finalize_previous();
	have_prev = false;

	++total_calls;

	EdgeKey key = {};
	key.from    = static_cast<uint32_t>(from_cs) * 16 + from_ip;
	key.to      = static_cast<uint32_t>(to_cs) * 16 + to_ip;
	key.kind    = static_cast<uint8_t>(kind);

	// Records are deduplicated per edge, so only one execution's arguments
	// and instruction history can be kept. Keeping only the first means a
	// function called thousands of times is sampled exactly once, at the
	// least interesting moment -- during startup rather than during the
	// gameplay the capture was armed for. Instead the sample is refreshed
	// whenever the hit count reaches a power of two, which costs a bit test
	// per call, spreads samples across the whole run, and guarantees the
	// stored one is from late in it. sample_hits records which execution
	// the stored sample came from.
	auto it = edges.find(key);
	if (it != edges.end()) {
		CallRecord& existing = it->second;
		++existing.count;
		const uint64_t n = existing.count;
		if ((n & (n - 1)) != 0) {
			return;
		}
	}

	CallRecord rec = {};
	rec.count      = (it != edges.end()) ? it->second.count : 1;
	rec.sample_hits = rec.count;
	rec.kind       = kind;
	rec.from_cs    = from_cs;
	rec.from_ip    = from_ip;
	rec.to_cs      = to_cs;
	rec.to_ip      = to_ip;
	rec.ds = SegValue(ds); rec.es = SegValue(es); rec.ss = SegValue(ss);
	rec.fs = SegValue(fs); rec.gs = SegValue(gs);

	for (int i = 0; i < NumGpRegs; ++i) {
		rec.regs[i]     = read_gp(i);
		rec.reg_args[i] = report_for(rec.regs[i], reg_prov[i]);
	}

	const auto sp_base = static_cast<PhysPt>(SegPhys(ss) +
	                                         (reg_esp & cpu.stack.mask));
	for (int i = 0; i < CallrecStackWords; ++i) {
		uint16_t w = 0;
		if (mem_readw_checked(sp_base + static_cast<PhysPt>(i * 2), &w)) {
			w = 0xFFFF;
		}
		rec.stack[i] = w;
	}

	// Stack slots get their provenance from the shadow: the write event
	// that last touched the slot is the PUSH, and the event carries the
	// pushed value's own origin.
	for (int i = 0; i < CallrecStackArgs; ++i) {
		const uint32_t addr = sp_base + static_cast<uint32_t>(i * 2);
		ArgReport r         = {};
		r.value             = rec.stack[i];

		ProvStatus st          = ProvStatus::Unknown;
		uint32_t last_write    = 0;
		const WriteEvent* ev   = nullptr;
		shadow_query(addr, 2, st, last_write, &ev);

		r.has_mem        = true;
		r.mem_addr       = addr;
		r.mem_status     = st;
		r.mem_last_write = last_write;

		if (ev && st == ProvStatus::Exact) {
			r.call_prov.last_writer_addr = ev->insn_addr;
			r.call_prov.origin_addr      = ev->origin_addr;
			r.call_prov.origin_kind      = ev->origin_kind;
			r.call_prov.mem_addr         = ev->origin_mem;
			r.call_prov.status           = ev->origin_status;
		}
		rec.stack_args[i] = r;
	}

	// Attach the tail of the instruction history, newest last. This is the
	// human-readable cross-check on the provenance above: in most real
	// cases "MOV AX,[SI+50]; PUSH AX; CALL X" is self-evident on sight.
	const uint32_t have = static_cast<uint32_t>(
	        std::min<uint64_t>(ring_total, CallrecRingAttached));
	rec.ring_snapshot_len = have;
	for (uint32_t i = 0; i < have; ++i) {
		const uint32_t idx = (ring_pos + CallrecRingEntries - have + i) %
		                     CallrecRingEntries;
		rec.ring_snapshot[i] = ring[idx];
	}

	edges[key] = rec;
}

// ***************************************************************************
// Control and dump
// ***************************************************************************

void CALLREC_Reset()
{
	edges.clear();
	total_calls = 0;
	ring_pos    = 0;
	ring_total  = 0;
	memset(ring, 0, sizeof(ring));
	invalidate_all_regs();
	have_prev      = false;
	cur_insn       = InsnInfo{};
	cur_insn_addr  = 0;
	cur_read_valid = false;
	shadow_init();
}

void CALLREC_SetEnabled(const bool enabled)
{
	if (enabled && !g_callrec_enabled) {
		CALLREC_Reset();
	}
	g_callrec_enabled = enabled;
}

bool CALLREC_IsEnabled()
{
	return g_callrec_enabled;
}

void CALLREC_GetStats(uint32_t& edge_count, uint32_t& call_sites,
                      uint64_t& calls)
{
	edge_count = static_cast<uint32_t>(edges.size());
	calls      = total_calls;

	std::unordered_map<uint32_t, char> sites;
	for (const auto& [key, rec] : edges) {
		sites[key.from] = 1;
	}
	call_sites = static_cast<uint32_t>(sites.size());
}

static const char* kind_name(const CallrecKind k)
{
	switch (k) {
	case CallrecKind::NearDirect: return "near_direct";
	case CallrecKind::NearIndirect: return "near_indirect";
	case CallrecKind::FarDirect: return "far_direct";
	default: return "far_indirect";
	}
}

static std::string json_prov(const Provenance& p)
{
	char buf[256];
	snprintf(buf,
	         sizeof(buf),
	         "{\"status\":\"%s\",\"origin_insn\":%u,\"origin_kind\":\"%s\","
	         "\"last_writer\":%u,\"read_size\":%u,\"partial\":%s,"
	         "\"partial_half\":\"%s\"}",
	         status_name(p.status),
	         p.origin_addr,
	         origin_name(p.origin_kind),
	         p.last_writer_addr,
	         p.mem_size,
	         p.partial ? "true" : "false",
	         p.partial ? (p.high_byte ? "high" : "low") : "");
	return buf;
}

static std::string json_arg(const char* name, const ArgReport& a)
{
	std::string s = "{\"name\":\"";
	s += name;
	char buf[256];
	snprintf(buf, sizeof(buf), "\",\"value\":%u,\"call_provenance\":", a.value);
	s += buf;
	s += json_prov(a.call_prov);
	if (a.has_mem) {
		snprintf(buf,
		         sizeof(buf),
		         ",\"memory_provenance\":{\"address\":%u,\"status\":\"%s\","
		         "\"last_write\":%u}",
		         a.mem_addr,
		         status_name(a.mem_status),
		         a.mem_last_write);
		s += buf;
	}
	s += "}";
	return s;
}

int CALLREC_Dump()
{
	const std_fs::path out_path = "CALLREC.JSONL";
	FILE* f = fopen(out_path.string().c_str(), "wb");
	if (!f) {
		return -1;
	}

	fprintf(f,
	        "{\"record\":\"header\",\"version\":1,\"stack_words\":%d,"
	        "\"stack_args\":%d,\"ring_attached\":%d,\"total_calls\":%llu,"
	        "\"edges\":%zu}\n",
	        CallrecStackWords,
	        CallrecStackArgs,
	        CallrecRingAttached,
	        static_cast<unsigned long long>(total_calls),
	        edges.size());

	// Call-site dispatch distribution. Purely a regrouping of the edges
	// below, emitted because it is the shape that answers "is this a real
	// polymorphic dispatch point, and how does its traffic split".
	std::unordered_map<uint32_t, std::vector<const CallRecord*>> by_site;
	for (const auto& [key, rec] : edges) {
		by_site[key.from].push_back(&rec);
	}
	for (const auto& [site, recs] : by_site) {
		fprintf(f,
		        "{\"record\":\"callsite\",\"site\":%u,\"target_count\":%zu,"
		        "\"targets\":[",
		        site,
		        recs.size());
		bool first = true;
		for (const auto* r : recs) {
			fprintf(f,
			        "%s{\"target\":%u,\"target_cs_ip\":\"%04X:%04X\","
			        "\"kind\":\"%s\",\"hits\":%llu}",
			        first ? "" : ",",
			        static_cast<uint32_t>(r->to_cs) * 16 + r->to_ip,
			        r->to_cs,
			        r->to_ip,
			        kind_name(r->kind),
			        static_cast<unsigned long long>(r->count));
			first = false;
		}
		fprintf(f, "]}\n");
	}

	int written = 0;
	for (const auto& [key, rec] : edges) {
		fprintf(f,
		        "{\"record\":\"call\",\"kind\":\"%s\",\"hits\":%llu,"
		        "\"sample_from_hit\":%llu,"
		        "\"from\":%u,\"from_cs_ip\":\"%04X:%04X\","
		        "\"to\":%u,\"to_cs_ip\":\"%04X:%04X\"",
		        kind_name(rec.kind),
		        static_cast<unsigned long long>(rec.count),
		        static_cast<unsigned long long>(rec.sample_hits),
		        key.from,
		        rec.from_cs,
		        rec.from_ip,
		        key.to,
		        rec.to_cs,
		        rec.to_ip);

		fprintf(f,
		        ",\"segments\":{\"ds\":%u,\"es\":%u,\"ss\":%u,\"fs\":%u,\"gs\":%u}",
		        rec.ds, rec.es, rec.ss, rec.fs, rec.gs);

		fprintf(f, ",\"registers\":[");
		for (int i = 0; i < NumGpRegs; ++i) {
			fprintf(f,
			        "%s%s",
			        i ? "," : "",
			        json_arg(RegNames[i], rec.reg_args[i]).c_str());
		}
		fprintf(f, "]");

		fprintf(f, ",\"stack_args\":[");
		for (int i = 0; i < CallrecStackArgs; ++i) {
			char nm[16];
			snprintf(nm, sizeof(nm), "sp+%d", i * 2);
			fprintf(f,
			        "%s%s",
			        i ? "," : "",
			        json_arg(nm, rec.stack_args[i]).c_str());
		}
		fprintf(f, "]");

		fprintf(f, ",\"stack\":[");
		for (int i = 0; i < CallrecStackWords; ++i) {
			fprintf(f, "%s%u", i ? "," : "", rec.stack[i]);
		}
		fprintf(f, "]");

		fprintf(f, ",\"recent_instructions\":[");
		for (uint32_t i = 0; i < rec.ring_snapshot_len; ++i) {
			const RingEntry& e = rec.ring_snapshot[i];
			fprintf(f, "%s{\"addr\":%u,\"code\":\"", i ? "," : "", e.addr);
			for (int b = 0; b < CallrecRingCodeBytes; ++b) {
				fprintf(f, "%02X", e.code[b]);
			}
			fprintf(f,
			        "\",\"read_addr\":%u,\"read_size\":%u,"
			        "\"write_addr\":%u,\"write_size\":%u",
			        e.read_addr,
			        e.read_size,
			        e.write_addr,
			        e.write_size);
			fprintf(f, ",\"regs\":[");
			for (int r = 0; r < NumGpRegs; ++r) {
				fprintf(f, "%s%u", r ? "," : "", e.regs[r]);
			}
			fprintf(f, "]}");
		}
		fprintf(f, "]}\n");
		++written;
	}

	fclose(f);
	return written;
}

#endif // C_DEBUGGER
