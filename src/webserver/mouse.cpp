// SPDX-FileCopyrightText:  2026-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "webserver.h"
#include "bridge.h"
#include "private/mouse.h"

#include <queue>
#include <stdexcept>
#include <string>

#include "hardware/input/mouse.h"
#include "hardware/pic.h"
#include "json/json.h"

using json = nlohmann::json;

namespace Webserver {

// --- Move -----------------------------------------------------------------

void MouseMoveCommand::Execute()
{
	if (absolute) {
		// Deterministic path: no gating, no sensitivity scaling. Fails
		// only if there is no DOS mouse driver to talk to.
		delivered = MOUSE_SetDosPosition(static_cast<uint16_t>(x),
		                                 static_cast<uint16_t>(y));
		if (!delivered) {
			block_reason = "No DOS mouse driver is resident";
		}
	} else {
		block_reason = MOUSE_GetInjectionBlockReason(false);
		delivered    = (block_reason == nullptr);
		MOUSE_InjectMotionRelative(x, y);
	}

	has_position = MOUSE_GetDosPosition(pos_x, pos_y);
}

void MouseMoveCommand::Post(const httplib::Request& req, httplib::Response& res)
{
	auto j = json::parse(req.body);

	const bool has_abs = j.contains("x") || j.contains("y");
	const bool has_rel = j.contains("dx") || j.contains("dy");

	if (has_abs && has_rel) {
		throw std::invalid_argument(
		        "Give either x/y (absolute) or dx/dy (relative), not both");
	}
	if (!has_abs && !has_rel) {
		throw std::invalid_argument(
		        "Missing required fields: x and y (absolute), or dx and dy (relative)");
	}

	float x = 0;
	float y = 0;
	if (has_abs) {
		if (!j.contains("x") || !j.contains("y")) {
			throw std::invalid_argument("Absolute move needs both x and y");
		}
		x = j.at("x").get<float>();
		y = j.at("y").get<float>();
		if (x < 0 || y < 0) {
			throw std::invalid_argument("x and y must not be negative");
		}
	} else {
		if (!j.contains("dx") || !j.contains("dy")) {
			throw std::invalid_argument("Relative move needs both dx and dy");
		}
		x = j.at("dx").get<float>();
		y = j.at("dy").get<float>();
	}

	MouseMoveCommand cmd(has_abs, x, y);
	cmd.WaitForCompletion();

	json out;
	out["mode"]      = has_abs ? "absolute" : "relative";
	out["delivered"] = cmd.delivered;
	if (cmd.block_reason) {
		out["reason"] = std::string(cmd.block_reason);
	}
	if (cmd.has_position) {
		out["position"] = {{"x", cmd.pos_x}, {"y", cmd.pos_y}};
	}
	send_json(res, out);
}

// --- Button ---------------------------------------------------------------

// Buttons awaiting a scheduled release, in the order they were queued. One
// PIC event is armed per entry and they fire in order, so this stays in step.
static std::queue<MouseButtonId> pending_releases = {};

static void release_pending_button(uint32_t /* unused */)
{
	if (pending_releases.empty()) {
		return;
	}
	MOUSE_InjectButton(pending_releases.front(), false);
	pending_releases.pop();
}

void MouseButtonCommand::Execute()
{
	// A release is never gated by the emulation (deliberately, so a button
	// cannot get stuck down), only a press is.
	if (action == Action::Up) {
		MOUSE_InjectButton(button_id, false);
		delivered = true;
		return;
	}

	block_reason = MOUSE_GetInjectionBlockReason(true);
	delivered    = (block_reason == nullptr);

	MOUSE_InjectButton(button_id, true);

	if (action == Action::Click) {
		pending_releases.push(button_id);
		PIC_AddEvent(release_pending_button, hold_ms, 0);
	}
}

static MouseButtonId parse_button(const std::string& name)
{
	if (name == "left") {
		return MouseButtonId::Left;
	}
	if (name == "right") {
		return MouseButtonId::Right;
	}
	if (name == "middle") {
		return MouseButtonId::Middle;
	}
	if (name == "extra1") {
		return MouseButtonId::Extra1;
	}
	if (name == "extra2") {
		return MouseButtonId::Extra2;
	}
	throw std::invalid_argument(
	        "Invalid button '" + name +
	        "', expected one of: left, right, middle, extra1, extra2");
}

void MouseButtonCommand::Post(const httplib::Request& req, httplib::Response& res)
{
	auto j = json::parse(req.body);

	if (!j.contains("button") || !j.at("button").is_string()) {
		throw std::invalid_argument("Missing required field: button");
	}
	const auto button_id = parse_button(j.at("button").get<std::string>());

	const auto action_name = j.value("action", std::string("click"));
	Action action          = Action::Click;
	if (action_name == "down") {
		action = Action::Down;
	} else if (action_name == "up") {
		action = Action::Up;
	} else if (action_name != "click") {
		throw std::invalid_argument(
		        "Invalid action '" + action_name +
		        "', expected one of: down, up, click");
	}

	const auto hold_ms = j.value("hold_ms", DefaultButtonHoldMs);

	MouseButtonCommand cmd(button_id, action, hold_ms);
	cmd.WaitForCompletion();

	json out;
	out["button"]    = j.at("button").get<std::string>();
	out["action"]    = action_name;
	out["delivered"] = cmd.delivered;
	if (action == Action::Click) {
		out["hold_ms"] = hold_ms;
	}
	if (cmd.block_reason) {
		out["reason"] = std::string(cmd.block_reason);
	}
	send_json(res, out);
}

// --- Status ---------------------------------------------------------------

void MouseStatusCommand::Execute()
{
	has_position       = MOUSE_GetDosPosition(pos_x, pos_y);
	move_block_reason  = MOUSE_GetInjectionBlockReason(false);
	press_block_reason = MOUSE_GetInjectionBlockReason(true);
}

void MouseStatusCommand::Get(const httplib::Request&, httplib::Response& res)
{
	MouseStatusCommand cmd;
	cmd.WaitForCompletion();

	json out;
	out["dos_driver"] = cmd.has_position;
	if (cmd.has_position) {
		out["position"] = {{"x", cmd.pos_x}, {"y", cmd.pos_y}};
	}
	out["accepts_relative_move"] = (cmd.move_block_reason == nullptr);
	out["accepts_button_press"]  = (cmd.press_block_reason == nullptr);
	if (cmd.move_block_reason) {
		out["move_blocked_reason"] = std::string(cmd.move_block_reason);
	}
	if (cmd.press_block_reason) {
		out["press_blocked_reason"] = std::string(cmd.press_block_reason);
	}
	send_json(res, out);
}

} // namespace Webserver
