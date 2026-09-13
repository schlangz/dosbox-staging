// SPDX-FileCopyrightText:  2026-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_WEBSERVER_KEYBOARD_H
#define DOSBOX_WEBSERVER_KEYBOARD_H

#include "webserver/bridge.h"

#include <cstdint>
#include <string>
#include <vector>

#include "http/http.h"
#include "json/json.h"

namespace Webserver {

// How long a key is held down by default, in emulated milliseconds. Matches
// AUTOTYPE's own press-to-release gap. A zero-length hold is invisible to
// software that samples key state once per frame, which is why this is not
// simply "press then release immediately".
constexpr uint32_t DefaultKeyHoldMs = 50;

// Gap between successive modifier releases in a chord, so they come back up
// after the key itself and in reverse order.
constexpr uint32_t ModifierReleaseStaggerMs = 5;

// Presses a single physical key, optionally with modifiers held around it
// (e.g. key="x", modifiers=["lalt"] for Alt+X), and schedules the releases
// hold_ms of emulated time later.
//
// This is the raw scancode path: the guest sees a real IRQ1/INT 9 scancode
// pair, so it reaches software that reads the keyboard directly. Button
// names are PHYSICAL KEY POSITIONS, not characters -- the guest's own
// keyboard layout decides which character a position produces, so "z" is
// 'y' on a German layout. Use KeyboardTypeCommand to enter characters.
class KeyboardKeyCommand : public Command {
public:
	KeyboardKeyCommand(std::string key, std::vector<std::string> modifiers,
	                   const uint32_t hold_ms)
	        : key(std::move(key)),
	          modifiers(std::move(modifiers)),
	          hold_ms(hold_ms)
	{}

	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	std::string key;
	std::vector<std::string> modifiers;
	uint32_t hold_ms = DefaultKeyHoldMs;

	// Filled in by Execute() so the response can say whether the key
	// genuinely reached the guest instead of just reporting HTTP 200.
	uint32_t dropped      = 0;
	bool accepting_before = false;
	// Non-null when the emulation is parked, so nothing will consume the
	// injected event until it resumes. See input_block_reason().
	const char* block_reason = nullptr;
};

// Types a run of text by writing characters straight into the BIOS keyboard
// ring buffer, the same buffer INT 16h reads from. Layout-independent: the
// requested character is the character the guest receives, and every
// printable ASCII character works, shifted symbols included.
//
// Reaches anything reading keys through INT 16h / DOS (the shell,
// text-mode programs). Does NOT reach software with its own INT 9 handler
// reading scancodes directly -- use KeyboardKeyCommand for that.
//
// Returns once queued. The BIOS ring holds 15 entries, so longer text is
// drip-fed as the guest consumes it.
class KeyboardTypeCommand : public Command {
public:
	KeyboardTypeCommand(std::string text, const uint32_t wait_ms)
	        : text(std::move(text)),
	          wait_ms(wait_ms)
	{}

	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	std::string text;
	uint32_t wait_ms = 0;

	uint32_t queued  = 0;
	uint32_t backlog = 0;
	// Characters with no BIOS-buffer representation, reported back rather
	// than silently skipped.
	std::string unsupported = {};
	// Non-null when the emulation is parked, so the queue this fills will
	// not drain until it resumes. See input_block_reason().
	const char* block_reason = nullptr;
};

// Why injected input will not reach the guest right now, or nullptr when it
// will. Emulation thread only.
const char* input_block_reason();

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_KEYBOARD_H
