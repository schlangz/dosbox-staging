// SPDX-FileCopyrightText:  2026-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "webserver.h"
#include "bridge.h"
#include "private/keyboard.h"

#include <cctype>
#include <queue>
#include <stdexcept>

#include "debugger/debugger.h"
#include "dosbox.h"
#include "gui/mapper.h"
#include "hardware/input/keyboard.h"
#include "hardware/pic.h"
#include "ints/bios.h"
#include "json/json.h"

using json = nlohmann::json;

namespace Webserver {

// Injected input reaches the guest through the PIC event queue and the guest's
// own interrupt handling, neither of which advances unless the CPU is running.
// A press queued while the machine is parked sits there, undelivered, until it
// resumes, so reporting the enqueue as delivery would be a lie.
//
// Two ways to be parked: the emulator is paused (which includes the automatic
// pause on focus loss, the usual state when driving DOSBox from another
// process), or the interactive debugger has the CPU stopped.
const char* input_block_reason()
{
	if (DOSBOX_IsPaused()) {
		return "The emulator is paused, so no injected input reaches the "
		       "guest until it resumes (note that losing window focus "
		       "auto-pauses by default)";
	}
	if (DOSBOX_IsPauseRequested()) {
		return "The emulator is pausing, so injected input will not reach "
		       "the guest until it resumes";
	}
	if (DEBUG_IsStopped()) {
		return "The CPU is stopped in the debugger, so no injected input "
		       "reaches the guest until execution resumes";
	}
	return nullptr;
}

// --- /keyboard/key: raw scancode injection --------------------------------
// Goes through the mapper, so the guest sees a genuine IRQ1/INT 9 scancode
// pair. This is the path that reaches software reading the keyboard
// directly (games with their own INT 9 handler), and it is subject to the
// guest's keyboard layout: a button name here is a *physical key position*,
// not a character. "z" on a German guest layout produces 'y'.
//
// The release is scheduled a real, non-zero span of emulated time after the
// press. Pressing and releasing back to back within one Execute() -- what
// this route did originally -- gives the key a zero-length hold, which
// software that samples key state once per frame never observes as held.

void KeyboardKeyCommand::Execute()
{
	const auto dropped_before  = KEYBOARD_GetDroppedKeyCount();
	accepting_before           = KEYBOARD_IsAcceptingInput();
	block_reason               = input_block_reason();

	for (const auto& m : modifiers) {
		if (!MAPPER_PressKey(m, true)) {
			error = "Unknown modifier button name: " + m;
			return;
		}
	}
	if (!MAPPER_PressKey(key, true)) {
		// Release anything already held, so a bad key name cannot leave
		// a modifier stuck down.
		for (auto it = modifiers.rbegin(); it != modifiers.rend(); ++it) {
			MAPPER_PressKey(*it, false);
		}
		error = "Unknown key button name: " + key;
		return;
	}

	MAPPER_ScheduleRelease(key, hold_ms);

	// Modifiers are released after the key itself, staggered so the PIC
	// event order matches the queue order the mapper releases them in.
	uint32_t release_delay_ms = hold_ms;
	for (auto it = modifiers.rbegin(); it != modifiers.rend(); ++it) {
		release_delay_ms += ModifierReleaseStaggerMs;
		MAPPER_ScheduleRelease(*it, release_delay_ms);
	}

	// The emulated keyboard silently discards keys when the guest has
	// disabled scanning or its scancode buffer has overflowed. Report that
	// honestly rather than returning a bare 200 for a key that never
	// arrived.
	dropped = KEYBOARD_GetDroppedKeyCount() - dropped_before;
}

void KeyboardKeyCommand::Post(const httplib::Request& req, httplib::Response& res)
{
	auto j = json::parse(req.body);

	if (!j.contains("key") || !j.at("key").is_string()) {
		throw std::invalid_argument("Missing required field: key");
	}
	auto key = j.at("key").get<std::string>();

	std::vector<std::string> modifiers;
	if (j.contains("modifiers")) {
		modifiers = j.at("modifiers").get<std::vector<std::string>>();
	}

	const auto hold_ms = j.value("hold_ms", DefaultKeyHoldMs);

	KeyboardKeyCommand cmd(key, modifiers, hold_ms);
	cmd.WaitForCompletion();

	if (!cmd.error.empty()) {
		throw std::invalid_argument(cmd.error);
	}

	json out;
	out["pressed"]   = key;
	out["hold_ms"]  = hold_ms;
	out["delivered"] = (cmd.dropped == 0) && cmd.accepting_before &&
	                   (cmd.block_reason == nullptr);
	if (cmd.dropped) {
		out["dropped_keys"] = cmd.dropped;
	}
	if (cmd.block_reason) {
		out["reason"] = std::string(cmd.block_reason);
	} else if (!cmd.accepting_before) {
		out["reason"] = "Emulated keyboard is not accepting input (guest "
		                "disabled scanning, or its scancode buffer overflowed)";
	}
	send_json(res, out);
}

// --- /keyboard/type: character injection ----------------------------------
// Text entry does NOT go through the mapper. The mapper delivers physical
// key positions, which the guest's own keyboard layout then translates, so
// asking for 'z' produced 'y' on a German layout and every shifted symbol
// (: ! ? " and friends) was silently dropped because no unshifted key
// position produces it. Both failures returned HTTP 200.
//
// Instead the character is written straight into the BIOS keyboard ring
// buffer at 0040:001E, the same buffer INT 16h reads from, and the same
// mechanism INT 16h AH=05h ("keyboard write") already exposes to real DOS
// programs. That is layout-independent by construction: the requested
// character is the character the guest receives.
//
// The tradeoff, stated plainly: this reaches everything that reads keys
// through INT 16h / DOS (the shell, text-mode programs), and does NOT
// reach software with its own INT 9 handler that reads scancodes directly.
// Use /keyboard/key for that.

// US scancode (set 1) of the physical key that produces this character on a
// US layout. Only the low ASCII byte of a BIOS buffer word drives DOS text
// input, but some software inspects the scancode byte too, so fill it in
// where a sensible answer exists. 0 means "no matching key".
static uint8_t us_scancode_for(const char c)
{
	switch (c) {
	case 0x1B: return 0x01; // esc
	case '1': case '!': return 0x02;
	case '2': case '@': return 0x03;
	case '3': case '#': return 0x04;
	case '4': case '$': return 0x05;
	case '5': case '%': return 0x06;
	case '6': case '^': return 0x07;
	case '7': case '&': return 0x08;
	case '8': case '*': return 0x09;
	case '9': case '(': return 0x0A;
	case '0': case ')': return 0x0B;
	case '-': case '_': return 0x0C;
	case '=': case '+': return 0x0D;
	case '\b': return 0x0E;
	case '\t': return 0x0F;
	case 'q': case 'Q': return 0x10;
	case 'w': case 'W': return 0x11;
	case 'e': case 'E': return 0x12;
	case 'r': case 'R': return 0x13;
	case 't': case 'T': return 0x14;
	case 'y': case 'Y': return 0x15;
	case 'u': case 'U': return 0x16;
	case 'i': case 'I': return 0x17;
	case 'o': case 'O': return 0x18;
	case 'p': case 'P': return 0x19;
	case '[': case '{': return 0x1A;
	case ']': case '}': return 0x1B;
	case '\r': return 0x1C;
	case 'a': case 'A': return 0x1E;
	case 's': case 'S': return 0x1F;
	case 'd': case 'D': return 0x20;
	case 'f': case 'F': return 0x21;
	case 'g': case 'G': return 0x22;
	case 'h': case 'H': return 0x23;
	case 'j': case 'J': return 0x24;
	case 'k': case 'K': return 0x25;
	case 'l': case 'L': return 0x26;
	case ';': case ':': return 0x27;
	case '\'': case '"': return 0x28;
	case '`': case '~': return 0x29;
	case '\\': case '|': return 0x2B;
	case 'z': case 'Z': return 0x2C;
	case 'x': case 'X': return 0x2D;
	case 'c': case 'C': return 0x2E;
	case 'v': case 'V': return 0x2F;
	case 'b': case 'B': return 0x30;
	case 'n': case 'N': return 0x31;
	case 'm': case 'M': return 0x32;
	case ',': case '<': return 0x33;
	case '.': case '>': return 0x34;
	case '/': case '?': return 0x35;
	case ' ': return 0x39;
	default: return 0;
	}
}

// Characters still to be pushed. The BIOS ring buffer holds only 15
// entries, so anything longer is drip-fed as the guest consumes it rather
// than silently truncated at the buffer boundary.
static std::queue<uint16_t> pending_chars = {};
static bool drain_event_armed             = false;

constexpr uint32_t DrainIntervalMs = 20;

static void drain_pending_chars(uint32_t /* unused */)
{
	drain_event_armed = false;

	while (!pending_chars.empty()) {
		if (!BIOS_AddKeyToBuffer(pending_chars.front())) {
			break; // ring is full, try again next interval
		}
		pending_chars.pop();
	}

	if (!pending_chars.empty()) {
		drain_event_armed = true;
		PIC_AddEvent(drain_pending_chars, DrainIntervalMs, 0);
	}
}

void KeyboardTypeCommand::Execute()
{
	block_reason = input_block_reason();

	for (const char c : text) {
		// '\n' and '\r' both mean Enter to a DOS program.
		const char ch = (c == '\n') ? '\r' : c;

		const auto scancode = us_scancode_for(ch);
		const auto is_printable = (static_cast<unsigned char>(ch) >= 0x20 &&
		                           static_cast<unsigned char>(ch) < 0x7F);
		const auto is_known_control = (ch == '\r' || ch == '\t' ||
		                               ch == '\b' || ch == 0x1B);

		if (!is_printable && !is_known_control) {
			unsupported += c;
			continue;
		}

		const auto code = static_cast<uint16_t>(
		        (static_cast<uint16_t>(scancode) << 8) |
		        static_cast<uint8_t>(ch));
		pending_chars.push(code);
		++queued;
	}

	// wait_ms lets a caller line the text up behind something the guest is
	// still finishing; otherwise start draining immediately.
	if (!drain_event_armed && !pending_chars.empty()) {
		drain_event_armed = true;
		PIC_AddEvent(drain_pending_chars, wait_ms, 0);
	}

	backlog = static_cast<uint32_t>(pending_chars.size());
}

void KeyboardTypeCommand::Post(const httplib::Request& req, httplib::Response& res)
{
	auto j = json::parse(req.body);

	if (!j.contains("text") || !j.at("text").is_string()) {
		throw std::invalid_argument("Missing required field: text");
	}
	auto text          = j.at("text").get<std::string>();
	const auto wait_ms = j.value("wait_ms", 0u);

	KeyboardTypeCommand cmd(text, wait_ms);
	cmd.WaitForCompletion();

	if (!cmd.error.empty()) {
		throw std::runtime_error(cmd.error);
	}

	json out;
	out["queued"] = cmd.queued;
	out["backlog"] = cmd.backlog;
	out["estimated_ms"] = wait_ms + cmd.backlog * DrainIntervalMs;
	// The characters are queued either way; this says whether anything is
	// going to consume that queue yet.
	out["draining"] = (cmd.block_reason == nullptr);
	if (cmd.block_reason) {
		out["reason"] = std::string(cmd.block_reason);
	}
	// Characters with no representation at all, reported rather than
	// dropped in silence.
	if (!cmd.unsupported.empty()) {
		out["unsupported"] = cmd.unsupported;
	}
	send_json(res, out);
}

} // namespace Webserver
