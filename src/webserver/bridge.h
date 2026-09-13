// SPDX-FileCopyrightText:  2026-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_WEBSERVER_BRIDGE_H
#define DOSBOX_WEBSERVER_BRIDGE_H

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace Webserver {

// How long a handler waits for the emulation thread to pick its command up.
//
// This is not a budget for the command itself: ProcessRequests() holds the
// queue mutex for the whole batch, so a waiter cannot wake to time out until
// the batch has finished and its own command is already done. What it bounds
// is how long the emulation thread may go without reaching a ProcessRequests()
// call site at all, which happens during an instruction-level CPU log, inside
// a long callback handler, or while the host blocks the main loop (a window
// drag on Windows enters a modal loop that does exactly that).
//
// Routes that a caller polls in a tight loop keep a short deadline so a stalled
// emulator is reported rather than hidden; routes that are expected to be
// issued into a busy emulator get a longer one.
constexpr uint32_t DefaultCommandTimeoutMs = 250;
constexpr uint32_t StatusCommandTimeoutMs  = 250;
constexpr uint32_t SlowCommandTimeoutMs    = 5000;

// Thrown when the emulation thread did not reach ProcessRequests() in time.
// Distinct from a command that ran and failed, which reports through
// Command::error, so the two map to different HTTP statuses.
class TimeoutError : public std::runtime_error {
public:
	explicit TimeoutError(const std::string& what) : std::runtime_error(what) {}
};

class Command {
public:
	virtual ~Command() {}
	virtual void Execute() = 0;

	void WaitForCompletion(const uint32_t timeout_ms = DefaultCommandTimeoutMs);

	// Set by Execute() to report errors without throwing on the
	// emulation thread. Handlers should check this after
	// WaitForCompletion() and throw on the webserver thread.
	//
	// An exception that escapes Execute() anyway is caught by the bridge
	// and lands here too, rather than unwinding the emulation thread.
	std::string error = {};

private:
	friend class Bridge;

	bool done = false;
};

class Bridge {
public:
	static Bridge& Instance();

	// Called by the web server thread
	void ExecuteCommand(Command& cmd, const uint32_t timeout_ms);

	// Called by the main thread running the CPU emulation.
	//
	// Invariant: nothing reachable from a Command::Execute() may call back
	// into this function. The queue mutex is not recursive, and a nested
	// machine loop (PAGING_PageFault() runs one) reaches the
	// ProcessRequests() call inside normal_loop(). Commands that touch
	// guest memory therefore use the MEM_Block*OutOfBand() accessors,
	// which cannot enter the page-fault core.
	void ProcessRequests();

private:
	std::mutex mtx                   = {};
	std::condition_variable cv       = {};
	std::vector<Command*> queue      = {};

	Bridge(const Bridge&)            = delete;
	Bridge& operator=(const Bridge&) = delete;
	Bridge()                         = default;
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_BRIDGE_H
