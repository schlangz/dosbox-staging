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

// Presses and releases a single key, optionally with modifiers held down
// around it (e.g. key="x", modifiers=["lalt"] for Alt+X). Delivered
// immediately/atomically, all within one Execute() -- for a single key
// or chord, not a run of text (see KeyboardTypeCommand for that).
class KeyboardKeyCommand : public Command {
public:
	KeyboardKeyCommand(std::string key, std::vector<std::string> modifiers)
	        : key(std::move(key)),
	          modifiers(std::move(modifiers))
	{}

	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	std::string key;
	std::vector<std::string> modifiers;
};

// Types a run of text via DOSBox's own AUTOTYPE mechanism (paced,
// PIC-timed key events) -- the same code path AUTOTYPE.COM uses, which
// avoids overrunning the emulated keyboard buffer. Returns immediately
// once queued; the actual typing happens over the following time as the
// PIC events fire.
class KeyboardTypeCommand : public Command {
public:
	KeyboardTypeCommand(std::string text, uint32_t wait_ms, uint32_t pace_ms)
	        : text(std::move(text)),
	          wait_ms(wait_ms),
	          pace_ms(pace_ms)
	{}

	void Execute() override;
	static void Post(const httplib::Request&, httplib::Response&);

private:
	std::string text;
	uint32_t wait_ms = 0;
	uint32_t pace_ms = 0;
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_KEYBOARD_H
