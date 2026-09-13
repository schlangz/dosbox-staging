// SPDX-FileCopyrightText:  2026-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "webserver.h"
#include "bridge.h"
#include "private/memory.h"

#include <cstdio>

#include "base64/base64.h"
#include "http/http.h"
#include "json/json.h"
#include "utils/string_utils.h"

// Spelled out in full: a bare "cpu.h" here resolves to this directory's own
// private/cpu.h, not the emulated CPU's header.
#include "cpu/cpu.h"
#include "cpu/registers.h"
#include "debugger/debugger.h"
#include "dos/dos_memory.h"
#include "hardware/memory.h"

using json = nlohmann::json;
using httplib::Request, httplib::Response;

namespace Webserver {

static Segment str_to_base_segment(const std::string_view str)
{
	auto val = upcase(str);
	static const std::unordered_map<std::string_view, Segment> lookup = {
	        {"CS", Segment::CS},
	        {"SS", Segment::SS},
	        {"DS", Segment::DS},
	        {"ES", Segment::ES},
	        {"FS", Segment::FS},
	        {"GS", Segment::GS},
	};

	if (auto it = lookup.find(val); it != lookup.end()) {
		return it->second;
	} else {
		return Segment::None;
	}
}

static uint32_t base_segment_to_offset(const Segment segment)
{
	switch (segment) {
	case Segment::CS: return SegPhys(SegNames::cs);
	case Segment::SS: return SegPhys(SegNames::ss);
	case Segment::DS: return SegPhys(SegNames::ds);
	case Segment::ES: return SegPhys(SegNames::es);
	case Segment::FS: return SegPhys(SegNames::fs);
	case Segment::GS: return SegPhys(SegNames::gs);
	case Segment::None: return 0;
	default: return 0;
	}
}

uint32_t resolve_mem_addr(const MemAddr& addr)
{
	switch (addr.base) {
	case Segment::None:
		// No segment given: the offset is already a linear address.
		return addr.offset;

	case Segment::Numeric:
		// A literal segment/selector number only means something
		// together with the CPU's current addressing mode, which is
		// why this cannot be folded into the offset at parse time.
		// Same resolver the interactive debugger uses, so a given
		// seg:offset reads the same bytes through either interface.
		return CPU_LinearAddressOf(addr.segment, addr.offset);

	default:
		// A segment register: its loaded descriptor cache already
		// holds the right base in every mode.
		return base_segment_to_offset(addr.base) + addr.offset;
	}
}

static MemAddr parse_mem_addr(const httplib::Request& req)
{
	MemAddr addr = {};

	addr.offset = num_param<uint32_t>(req, Source::Path, "offset");
	addr.base   = Segment::None;

	if (req.path_params.find("segment") != req.path_params.end()) {
		auto& segment_param = req.path_params.at("segment");
		addr.base           = str_to_base_segment(segment_param);

		// Not a register name, so it must be a literal segment or
		// protected-mode selector. It is kept as-is and resolved on
		// the emulation thread; folding it into the offset here would
		// hard-code real-mode addressing.
		if (addr.base == Segment::None) {
			addr.base = Segment::Numeric;
			addr.segment = num_param<uint16_t>(req, Source::Path, "segment");
		}
	}

	return addr;
}

// Hex spelling of an address, for messages that name one.
static std::string hex_addr(const uint32_t addr)
{
	char buf[16] = {};
	snprintf(buf, sizeof(buf), "0x%08x", addr);
	return buf;
}

// The linear address space is 32 bits wide, so a range may not wrap past its
// end. Checked before any access, since the accessors walk byte by byte and
// would silently restart at zero.
static bool range_fits_address_space(const uint32_t start, const uint64_t len)
{
	return static_cast<uint64_t>(start) + len <= 0x1'0000'0000ULL;
}

void ReadMemoryCommand::Execute()
{
	regs.load();
	effective_addr = resolve_mem_addr(addr);

	LOG_DEBUG("API: ReadMemoryCommand(0x%06x, %d)", effective_addr, len);

	if (!range_fits_address_space(effective_addr, len)) {
		error = "Address range " + hex_addr(effective_addr) + " + " +
		        std::to_string(len) +
		        " runs past the end of the 32-bit address space";
		return;
	}

	memory.resize(len);

	// Reading over HTTP is an observation, not guest execution: it must
	// neither be recorded as a memory-breakpoint hit nor page anything in
	// through the guest's own fault handler.
	const DEBUG_OutOfBandMemoryAccess out_of_band;

	PhysPt failed_at = 0;
	if (!MEM_BlockReadOutOfBand(effective_addr, memory.data(), len, &failed_at)) {
		memory.clear();
		error = "No memory is mapped at " + hex_addr(failed_at) +
		        " (in the range " + hex_addr(effective_addr) + " + " +
		        std::to_string(len) + ")";
		return;
	}
}

void ReadMemoryCommand::Get(const Request& req, Response& res)
{
	// 128 MiB per request ought to be enough for everyone.
	// This limit just prevents bad things when accidentally requesting an
	// unreasonably large size.
	auto num_bytes = num_param<uint32_t>(req, Source::Path, "len", 1, 128 * 1024 * 1024);

	ReadMemoryCommand cmd(parse_mem_addr(req), num_bytes);
	cmd.WaitForCompletion();

	if (!cmd.error.empty()) {
		throw std::out_of_range(cmd.error);
	}

	if (req.get_header_value("accept").starts_with(TypeJson)) {
		json j;
		j["registers"]      = cmd.regs;
		j["memory"]["addr"] = cmd.effective_addr;
		j["memory"]["data"] = base64::to_base64(cmd.memory);

		send_json(res, j);

	} else {
		// Only do base64 if explicitly requested, binary/download by
		// default.
		res.set_header("Content-Disposition",
		               "attachment; filename =\"memory.bin\"");
		res.set_content(cmd.memory, TypeBinary);
	}
}

void WriteMemoryCommand::Execute()
{
	regs.load();
	effective_addr = resolve_mem_addr(addr);

	LOG_DEBUG("API: WriteMemoryCommand(0x%06x, %d)", effective_addr, data.size());

	if (!range_fits_address_space(effective_addr, data.size())) {
		error = "Address range " + hex_addr(effective_addr) + " + " +
		        std::to_string(data.size()) +
		        " runs past the end of the 32-bit address space";
		return;
	}

	const DEBUG_OutOfBandMemoryAccess out_of_band;

	PhysPt failed_at = 0;

	if (!expected_data.empty()) {
		conflict_data.resize(expected_data.size());

		if (!MEM_BlockReadOutOfBand(effective_addr,
		                            conflict_data.data(),
		                            conflict_data.size(),
		                            &failed_at)) {
			conflict_data.clear();
			error = "No memory is mapped at " + hex_addr(failed_at) +
			        ", so the If-Match precondition cannot be checked";
			return;
		}

		if (expected_data != conflict_data) {
			return;
		}

		conflict_data.clear();
	}

	if (!MEM_BlockWriteOutOfBand(effective_addr,
	                             data.data(),
	                             data.size(),
	                             &failed_at)) {
		error = "No writable memory is mapped at " + hex_addr(failed_at) +
		        " (in the range " + hex_addr(effective_addr) + " + " +
		        std::to_string(data.size()) +
		        "); the bytes before it were written";
		DEBUG_ResyncMemoryBreakpoints(effective_addr,
		                              failed_at - effective_addr);
		return;
	}

	// Write-watching memory breakpoints report a hit by noticing the byte
	// differs from the value they last saw. This write is not the guest
	// changing it, so re-baseline the ones it covers.
	DEBUG_ResyncMemoryBreakpoints(effective_addr, data.size());
}

void WriteMemoryCommand::Put(const httplib::Request& req, httplib::Response& res)
{
	const auto addr = parse_mem_addr(req);

	constexpr size_t MaxWriteBytes = 128 * 1024 * 1024; // 128 MiB

	std::string data;

	const auto content_type = media_type_of(req.get_header_value("Content-Type"));

	if (content_type == TypeJson) {
		auto j = json::parse(req.body);
		data   = base64::from_base64(j.at("data").get<std::string>());

	} else if (content_type == TypeBinary) {
		data = req.body;

	} else {
		throw std::invalid_argument("Content-Type must be either " +
		                            std::string(TypeJson) + " or " +
		                            std::string(TypeBinary));
	}

	if (data.size() > MaxWriteBytes) {
		throw std::invalid_argument(
		        "Write data exceeds maximum size of 128 MiB");
	}

	std::string expected_data;

	if (req.has_header("If-Match")) {
		// The standard requires ETags in this and ETags are quoted
		// but we accept unquoted because no one is gonna bother
		std::string etag_hdr  = req.get_header_value("If-Match");
		std::string_view etag = etag_hdr;
		if (etag.starts_with('"') && etag.ends_with('"')) {
			etag.remove_prefix(1);
			etag.remove_suffix(1);
		}
		expected_data = base64::from_base64(etag);
	}

	WriteMemoryCommand cmd(addr, std::move(data), std::move(expected_data));
	cmd.WaitForCompletion();

	if (!cmd.error.empty()) {
		throw std::out_of_range(cmd.error);
	}

	json j;
	j["registers"]      = cmd.regs;
	j["memory"]["addr"] = cmd.effective_addr;
	if (!cmd.conflict_data.empty()) {
		res.status = httplib::StatusCode::PreconditionFailed_412;
		j["memory"]["data"] = base64::to_base64(cmd.conflict_data);
	}

	send_json(res, j);
}

} // namespace Webserver
