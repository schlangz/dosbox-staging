// SPDX-FileCopyrightText:  2026-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_WEBSERVER_MEMORY_H
#define DOSBOX_WEBSERVER_MEMORY_H

#include "webserver/bridge.h"
#include "cpu.h"

#include <limits>

#include "http/http.h"

namespace Webserver {

// How the request spelled the base of the address: as a segment register to
// read at the time the command runs, as a literal segment/selector number, or
// not at all (the offset is then already a linear address).
enum class Segment { None, Numeric, CS, SS, DS, ES, FS, GS };

// A literal segment number is only meaningful together with the CPU's current
// addressing mode, so it is resolved on the emulation thread inside Execute()
// rather than while parsing the request. Both memory commands therefore carry
// the raw number through.
struct MemAddr {
	Segment base = Segment::None;
	// Only meaningful when base == Segment::Numeric.
	uint16_t segment = 0;
	uint32_t offset  = 0;
};

// Resolves a parsed request address to a linear address. Emulation thread only.
uint32_t resolve_mem_addr(const MemAddr& addr);

class ReadMemoryCommand : public Command {
public:
	ReadMemoryCommand(const MemAddr addr, const uint32_t len)
	        : addr(addr),
	          len(len)
	{}

	void Execute() override;
	static void Get(const httplib::Request& req, httplib::Response& res);

private:
	// Request
	MemAddr addr    = {};
	uint32_t len    = {};

	// Response
	// Memory is std::string to avoid ugly casts for httplib
	std::string memory      = {};
	uint32_t effective_addr = {};
	Registers regs          = {};
};

class WriteMemoryCommand : public Command {
public:
	WriteMemoryCommand(const MemAddr addr, std::string data,
	                   std::string expected_data)
	        : addr(addr),
	          data(std::move(data)),
	          expected_data(std::move(expected_data))
	{}

	void Execute() override;
	static void Put(const httplib::Request& req, httplib::Response& res);

private:
	// Request
	MemAddr addr     = {};
	std::string data = {};
	// Only write the data if the current data at the address exactly
	// matches this. Usable as an atomic CAS to implement a mutex.
	std::string expected_data = {};

	// Response
	uint32_t effective_addr = {};
	// Only filled if expected_data was set and didn't match.
	std::string conflict_data = {};
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_MEMORY_H
