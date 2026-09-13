// SPDX-FileCopyrightText:  2026-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "webserver.h"
#include "bridge.h"
#include "private/screenshot.h"

#include <chrono>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "base64/base64.h"
#include "capture/capture.h"
#include "debugger/debugger.h"
#include "dosbox.h"
#include "json/json.h"

using json = nlohmann::json;

namespace Webserver {

void ScreenshotCommand::Execute()
{
	// A screenshot is taken from the next frame the renderer completes, and
	// frames only complete while the VGA emulation is being clocked. Neither
	// the debugger's loop nor the paused loop clocks it, so a request made
	// now would sit armed and fire at an unrelated later moment. Refusing is
	// the honest answer, and a specific one: the generic outcome otherwise
	// is a timeout that says nothing about why.
	if (DOSBOX_IsPaused()) {
		error = "No frames are being rendered while the emulator is paused, "
		        "so a screenshot cannot be taken (note that losing window "
		        "focus auto-pauses by default)";
		return;
	}
	if (DEBUG_IsStopped()) {
		error = "No frames are being rendered while the CPU is stopped in "
		        "the debugger, so a screenshot cannot be taken";
		return;
	}

	const auto index = CAPTURE_PeekNextImageIndex();
	path             = generate_capture_filename(CaptureType::RenderedImage, index);
	CAPTURE_RequestRenderedScreenshot();
}

void ScreenshotCommand::Get(const httplib::Request& req, httplib::Response& res)
{
	// The filename is predicted rather than reserved, so overlapping
	// requests would otherwise both wait on the same predicted path. One at
	// a time removes that half of the race; see the class comment for the
	// half that remains.
	static std::mutex screenshot_mutex = {};
	std::lock_guard<std::mutex> screenshot_lock(screenshot_mutex);

	ScreenshotCommand cmd;
	cmd.WaitForCompletion();

	if (!cmd.error.empty()) {
		throw std::runtime_error(cmd.error);
	}

	constexpr auto TimeoutMs      = 3000;
	constexpr auto PollIntervalMs = 25;
	constexpr auto SettleMs       = 100;

	auto waited = 0;
	while (!std_fs::exists(cmd.path) && waited < TimeoutMs) {
		std::this_thread::sleep_for(std::chrono::milliseconds(PollIntervalMs));
		waited += PollIntervalMs;
	}
	if (!std_fs::exists(cmd.path)) {
		throw std::runtime_error("Screenshot capture timed out waiting for " +
		                         cmd.path.string());
	}

	// The image saver writes the file on its own worker thread; guard
	// against reading a partial write by waiting for its size to settle.
	// The file can be replaced or removed underneath this, so the size is
	// read through the non-throwing overload, and the wait is bounded.
	auto current_size = [&cmd]() -> uintmax_t {
		std::error_code ec;
		const auto size = std_fs::file_size(cmd.path, ec);
		return ec ? 0 : size;
	};

	auto last_size = current_size();
	std::this_thread::sleep_for(std::chrono::milliseconds(SettleMs));

	auto settled_for = SettleMs;
	while (current_size() != last_size && settled_for < TimeoutMs) {
		last_size = current_size();
		std::this_thread::sleep_for(std::chrono::milliseconds(SettleMs));
		settled_for += SettleMs;
	}

	std::ifstream f(cmd.path, std::ios::binary);
	std::string data((std::istreambuf_iterator<char>(f)),
	                 std::istreambuf_iterator<char>());

	if (req.get_header_value("accept").starts_with(TypeJson)) {
		json j;
		j["image"]["path"] = cmd.path.string();
		j["image"]["data"] = base64::to_base64(data);
		send_json(res, j);
	} else {
		res.set_header("Content-Disposition",
		               "inline; filename=\"screenshot.png\"");
		res.set_content(data, "image/png");
	}
}

} // namespace Webserver
