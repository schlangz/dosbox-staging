// SPDX-FileCopyrightText:  2026-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_WEBSERVER_DEBUGGER_H
#define DOSBOX_WEBSERVER_DEBUGGER_H

#include "webserver/bridge.h"
#include "cpu.h"

#include <cstdint>
#include <string>
#include <vector>

#include "http/http.h"
#include "json/json.h"

namespace Webserver {

struct CodeLine {
	uint16_t seg     = 0;
	uint32_t off     = 0;
	std::string text = {};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CodeLine, seg, off, text)

// Reports whether the CPU is currently paused in the interactive
// debugger (from a breakpoint hit or a prior Enable/Step call) or
// running freely. Poll this after Go to detect the next real stop --
// there's no push notification, this is the wait-for-stop mechanism.
class DebuggerStatusCommand : public Command {
public:
	void Execute() override;
	static void Get(const httplib::Request&, httplib::Response&);

private:
	bool paused                = false;
	Registers regs              = {};
	std::vector<CodeLine> code = {};
};

// Pauses free-running emulation and enters the debugger, same as the
// interactive Alt+Pause hotkey.
class DebuggerEnableCommand : public Command {
public:
	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	Registers regs              = {};
	std::vector<CodeLine> code = {};
};

// Executes exactly one instruction. Only valid while paused.
class DebuggerStepCommand : public Command {
public:
	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	Registers regs              = {};
	std::vector<CodeLine> code = {};
};

// Resumes free-running emulation. Only valid while paused. Returns
// immediately -- poll DebuggerStatusCommand to see when it stops again.
class DebuggerGoCommand : public Command {
public:
	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);
};

class DebuggerAddBreakpointCommand : public Command {
public:
	DebuggerAddBreakpointCommand(const uint16_t seg, const uint32_t off)
	        : seg(seg),
	          off(off)
	{}

	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	uint16_t seg = 0;
	uint32_t off = 0;
};

class DebuggerDeleteBreakpointCommand : public Command {
public:
	DebuggerDeleteBreakpointCommand(const uint16_t seg, const uint32_t off)
	        : seg(seg),
	          off(off)
	{}

	void Execute() override;
	static void Delete(const httplib::Request&, httplib::Response&);

private:
	uint16_t seg  = 0;
	uint32_t off  = 0;
	bool removed = false;
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_DEBUGGER_H
