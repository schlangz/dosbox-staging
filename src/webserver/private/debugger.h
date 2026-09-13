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

// One poll of "has the CPU actually come to rest in the debugger", plus the
// state at that point. `stopped` follows DEBUG_IsStopped(), not
// DEBUG_IsDebugging(): a requested pause only becomes real once the running
// loop handler returns, and until then the guest is still executing.
class DebuggerPollStopCommand : public Command {
public:
	void Execute() override;

	bool stopped               = false;
	Registers regs             = {};
	std::vector<CodeLine> code = {};
};

// Reports whether the CPU is currently paused in the interactive
// debugger (from a breakpoint hit or a prior Enable/Step call) or
// running freely. Poll this after Go to detect the next real stop --
// there's no push notification, this is the wait-for-stop mechanism.
class DebuggerStatusCommand : public DebuggerPollStopCommand {
public:
	static void Get(const httplib::Request&, httplib::Response&);
};

// Pauses free-running emulation and enters the debugger, same as the
// interactive Alt+Pause hotkey. The response reports the state at the
// instruction the CPU actually stopped on, which the handler waits for.
class DebuggerEnableCommand : public Command {
public:
	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	bool stopped               = false;
	Registers regs             = {};
	std::vector<CodeLine> code = {};
};

// Executes exactly one instruction. Only valid while the CPU has actually
// stopped, not merely while a pause has been requested.
class DebuggerStepCommand : public Command {
public:
	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	Registers regs             = {};
	std::vector<CodeLine> code = {};
	// Set when the executed instruction produced something the caller
	// should know about, such as unwinding the machine loop. The
	// instruction still ran, so this is reported as a warning rather than
	// an error.
	std::string note = {};
};

// Resumes free-running emulation. Only valid while stopped. Returns
// immediately -- poll DebuggerStatusCommand to see when it stops again.
class DebuggerGoCommand : public Command {
public:
	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	std::string note = {};
};

// A breakpoint resolves seg:offset to a linear address once, when it is armed,
// and watches that address from then on. In protected mode a selector the
// descriptor tables do not describe has no such address, and
// CPU_LinearAddressOf() answers with the real-mode seg * 16 instead, so the
// breakpoint would sit at an unrelated location and never fire. `resolved`
// reports whether the stored `linear` means anything.
class DebuggerAddBreakpointCommand : public Command {
public:
	DebuggerAddBreakpointCommand(const uint16_t seg, const uint32_t off)
	        : seg(seg),
	          off(off)
	{}

	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	uint16_t seg   = 0;
	uint32_t off   = 0;
	uint32_t linear = 0;
	bool resolved  = false;
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
	uint16_t seg      = 0;
	uint32_t off      = 0;
	std::string label = {};
	// As for DebuggerAddBreakpointCommand: a logpoint watches the address
	// its seg:offset resolved to at arm time.
	uint32_t linear = 0;
	bool resolved   = false;
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

// Adds or removes a logsig: a logpoint identified by the executing code's
// own bytes rather than by an address resolved once at arm time, so it
// follows VROOMM-style relocatable overlay content instead of silently
// logging whatever later occupies the same physical slot.
//
// Body: {"signature": "558BEC...", "offset"?: 0, "label"?: "..."}. There is
// no address in the path because there is no fixed address at arm time.
// With a non-zero offset the watched point is entry+offset, re-resolved
// automatically whenever the overlay instance changes.
class DebuggerAddLogsigCommand : public Command {
public:
	DebuggerAddLogsigCommand(std::vector<uint8_t> signature,
	                         const uint32_t offset, std::string label)
	        : signature(std::move(signature)),
	          offset(offset),
	          label(std::move(label))
	{}

	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	std::vector<uint8_t> signature = {};
	uint32_t offset                = 0;
	std::string label              = {};
};

class DebuggerDeleteLogsigCommand : public Command {
public:
	DebuggerDeleteLogsigCommand(std::vector<uint8_t> signature, const uint32_t offset)
	        : signature(std::move(signature)),
	          offset(offset)
	{}

	void Execute() override;
	static void Delete(const httplib::Request&, httplib::Response&);

private:
	std::vector<uint8_t> signature = {};
	uint32_t offset                = 0;
	bool removed                   = false;
};

// Dynamic CALL recorder control. GET reports state and counts; POST with
// {"enabled": true|false} arms or disarms it; POST to .../dump writes
// CALLREC.JSONL. Works only under core=normal, by design.
class CallrecStatusCommand : public Command {
public:
	void Execute() override;
	static void Get(const httplib::Request&, httplib::Response&);

private:
	bool enabled          = false;
	uint32_t edges        = 0;
	uint32_t call_sites   = 0;
	uint64_t total_calls  = 0;
};

class CallrecSetCommand : public Command {
public:
	explicit CallrecSetCommand(const bool enable) : enable(enable) {}

	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	bool enable = false;
};

class CallrecDumpCommand : public Command {
public:
	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	int written = 0;
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
