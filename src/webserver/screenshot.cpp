// SPDX-FileCopyrightText:  2026-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "webserver.h"
#include "bridge.h"
#include "private/screenshot.h"

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>

#include "base64/base64.h"
#include "capture/capture.h"
#include "json/json.h"

using json = nlohmann::json;

namespace Webserver {

void ScreenshotCommand::Execute()
{
	const auto index = CAPTURE_PeekNextImageIndex();
	path             = generate_capture_filename(CaptureType::RenderedImage, index);
	CAPTURE_RequestRenderedScreenshot();
}

void ScreenshotCommand::Get(const httplib::Request& req, httplib::Response& res)
{
	ScreenshotCommand cmd;
	cmd.WaitForCompletion();

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
	auto last_size = std_fs::file_size(cmd.path);
	std::this_thread::sleep_for(std::chrono::milliseconds(SettleMs));
	while (std_fs::exists(cmd.path) && std_fs::file_size(cmd.path) != last_size) {
		last_size = std_fs::file_size(cmd.path);
		std::this_thread::sleep_for(std::chrono::milliseconds(SettleMs));
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
