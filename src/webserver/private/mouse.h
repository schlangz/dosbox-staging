// SPDX-FileCopyrightText:  2026-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_WEBSERVER_MOUSE_H
#define DOSBOX_WEBSERVER_MOUSE_H

#include "webserver/bridge.h"

#include <cstdint>
#include <string>

#include "hardware/input/mouse.h"
#include "http/http.h"
#include "json/json.h"

namespace Webserver {

// How long a mouse button is held down for a combined click, in emulated
// milliseconds. A zero-length hold is invisible to software that samples
// button state once per frame.
constexpr uint32_t DefaultButtonHoldMs = 50;

// Moves the guest cursor. Two mutually exclusive modes:
//
// - Absolute, body {"x": <int>, "y": <int>}: writes the DOS (INT 33h)
//   driver's cursor position directly, in the guest's own coordinate
//   space. Deterministic -- it bypasses mouse capture, window focus,
//   seamless mode and all sensitivity scaling, the same way the guest's
//   own INT 33h AX=04h call does. Requires a resident DOS mouse driver.
//   This is the mode to use to put the cursor somewhere specific.
//
// - Relative, body {"dx": <float>, "dy": <float>}: injects a real motion
//   event, so it reaches PS/2 and serial mouse emulation too, not only the
//   DOS driver. Sensitivity scaling still applies, so the deltas do NOT map
//   1:1 to guest pixels, and while the mouse is uncaptured the DOS driver
//   tracks the absolute position rather than these deltas -- use the
//   absolute mode above to move the DOS cursor.
class MouseMoveCommand : public Command {
public:
	MouseMoveCommand(const bool absolute, const float x, const float y)
	        : absolute(absolute),
	          x(x),
	          y(y)
	{}

	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	bool absolute = true;
	float x       = 0;
	float y       = 0;

	// Filled in by Execute() for an honest response.
	bool delivered           = false;
	bool has_position        = false;
	uint16_t pos_x           = 0;
	uint16_t pos_y           = 0;
	const char* block_reason = nullptr;
};

// Presses and/or releases a mouse button. Body:
//   {"button": "left"|"right"|"middle"|"extra1"|"extra2",
//    "action": "down"|"up"|"click",   (default "click")
//    "hold_ms": <int>}                (click only, default 50)
//
// "click" presses now and schedules the release hold_ms of emulated time
// later, so the button has a real hold duration.
class MouseButtonCommand : public Command {
public:
	enum class Action { Down, Up, Click };

	MouseButtonCommand(const MouseButtonId button_id, const Action action,
	                   const uint32_t hold_ms)
	        : button_id(button_id),
	          action(action),
	          hold_ms(hold_ms)
	{}

	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	MouseButtonId button_id = MouseButtonId::Left;
	Action action           = Action::Click;
	uint32_t hold_ms        = DefaultButtonHoldMs;

	bool delivered           = false;
	const char* block_reason = nullptr;
};

// Reports the DOS driver cursor position and whether injected events would
// currently reach the guest. The route to check before concluding that a
// click "did nothing".
class MouseStatusCommand : public Command {
public:
	void Execute() override;
	static void Get(const httplib::Request&, httplib::Response&);

private:
	bool has_position             = false;
	uint16_t pos_x                = 0;
	uint16_t pos_y                = 0;
	const char* move_block_reason = nullptr;
	const char* press_block_reason = nullptr;
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_MOUSE_H
