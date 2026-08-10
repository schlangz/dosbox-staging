// SPDX-FileCopyrightText:  2026-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "webserver.h"
#include "bridge.h"
#include "private/keyboard.h"

#include <cctype>
#include <stdexcept>

#include "gui/mapper.h"
#include "json/json.h"

using json = nlohmann::json;

namespace Webserver {

void KeyboardKeyCommand::Execute()
{
	for (const auto& m : modifiers) {
		MAPPER_PressKey(m, true);
	}
	MAPPER_PressKey(key, true);
	MAPPER_PressKey(key, false);
	for (auto it = modifiers.rbegin(); it != modifiers.rend(); ++it) {
		MAPPER_PressKey(*it, false);
	}
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

	KeyboardKeyCommand cmd(key, modifiers);
	cmd.WaitForCompletion();

	json out;
	out["pressed"] = key;
	send_json(res, out);
}

// Splits ASCII text into AUTOTYPE button names. Only covers unshifted
// printable ASCII plus space/enter/tab/backspace/esc -- shifted symbols
// (!@#$ etc) and non-ASCII text aren't handled. A literal ',' is also
// dropped rather than mistyped, since AUTOTYPE's own button list treats
// ',' as a pause marker, not a character.
static std::vector<std::string> TextToButtons(const std::string& text)
{
	std::vector<std::string> buttons;
	buttons.reserve(text.size());

	for (const unsigned char c : text) {
		switch (c) {
		case ' ': buttons.emplace_back("space"); continue;
		case '\n':
		case '\r': buttons.emplace_back("enter"); continue;
		case '\t': buttons.emplace_back("tab"); continue;
		case '\b': buttons.emplace_back("bspace"); continue;
		case 0x1B: buttons.emplace_back("esc"); continue;
		case '`': buttons.emplace_back("grave"); continue;
		case '-': buttons.emplace_back("minus"); continue;
		case '=': buttons.emplace_back("equals"); continue;
		case '[': buttons.emplace_back("lbracket"); continue;
		case ']': buttons.emplace_back("rbracket"); continue;
		case ';': buttons.emplace_back("semicolon"); continue;
		case '\'': buttons.emplace_back("quote"); continue;
		case '\\': buttons.emplace_back("backslash"); continue;
		case '.': buttons.emplace_back("period"); continue;
		case '/': buttons.emplace_back("slash"); continue;
		default: break;
		}
		if (std::isalnum(c)) {
			buttons.emplace_back(1, static_cast<char>(c));
		}
		// Anything else unsupported (shifted symbols, ',', non-ASCII)
		// is silently skipped rather than mistyped.
	}
	return buttons;
}

void KeyboardTypeCommand::Execute()
{
	auto buttons = TextToButtons(text);
	MAPPER_AutoType(buttons, wait_ms, pace_ms);
}

void KeyboardTypeCommand::Post(const httplib::Request& req, httplib::Response& res)
{
	auto j = json::parse(req.body);

	if (!j.contains("text") || !j.at("text").is_string()) {
		throw std::invalid_argument("Missing required field: text");
	}
	auto text          = j.at("text").get<std::string>();
	const auto wait_ms = j.value("wait_ms", 0u);
	const auto pace_ms = j.value("pace_ms", 150u);

	KeyboardTypeCommand cmd(text, wait_ms, pace_ms);
	cmd.WaitForCompletion();

	json out;
	out["queued"]        = true;
	out["estimated_ms"] = wait_ms + static_cast<uint32_t>(text.size()) * (pace_ms + 50);
	send_json(res, out);
}

} // namespace Webserver
