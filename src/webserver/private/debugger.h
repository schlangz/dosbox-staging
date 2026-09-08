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

// Adds a logpoint: on every execution of segment:offset one CPU state line
// is appended to LOGPOINTS.TXT and the guest keeps running at full speed,
// with no pause and no round trip back here. The optional JSON body field
// "label" names the logpoint in that file; it defaults to "LOGP". Cheap
// enough to leave armed at a handful of known addresses for a whole
// session, unlike the LOGL full instruction trace.
class DebuggerAddLogpointCommand : public Command {
public:
	DebuggerAddLogpointCommand(const uint16_t seg, const uint32_t off,
	                           std::string label)
	        : seg(seg),
	          off(off),
	          label(std::move(label))
	{}

	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	uint16_t seg = 0;
	uint32_t off = 0;
	std::string label = {};
};

class DebuggerDeleteLogpointCommand : public Command {
public:
	DebuggerDeleteLogpointCommand(const uint16_t seg, const uint32_t off)
	        : seg(seg),
	          off(off)
	{}

	void Execute() override;
	static void Delete(const httplib::Request&, httplib::Response&);

private:
	uint16_t seg = 0;
	uint32_t off = 0;
	bool removed = false;
};

// Runs an arbitrary interactive-debugger command (same syntax as typed at
// the DEBUG> prompt, e.g. "LOGL 7A120" or "BPM SS:1234") via the same
// ParseCommand() the UI uses. Only valid while paused -- this is the
// escape hatch for anything the typed routes above don't cover (memory
// breakpoints, CPU instruction logging, etc). Some commands (LOGL, GO,
// aliases) resume execution themselves; the response reflects whatever
// state the debugger is in immediately after the call returns.
class DebuggerCommandCommand : public Command {
public:
	explicit DebuggerCommandCommand(std::string command) : command(std::move(command)) {}

	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	std::string command;
	bool paused                = false;
	Registers regs              = {};
	std::vector<CodeLine> code = {};
};

// Toggles g_re_dump_enabled (cpu/cpu.h) -- the RE-instrumentation dump
// flag also toggled by the CTRL+F9 hotkey (see cpu.cpp's own
// handle_re_dump_toggle_event). Exposed here so it can be flipped
// remotely without needing focus on the DOSBox window.
class ReDumpToggleCommand : public Command {
public:
	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	bool enabled = false;
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_DEBUGGER_H
