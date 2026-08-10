// SPDX-FileCopyrightText:  2026-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_WEBSERVER_SCREENSHOT_H
#define DOSBOX_WEBSERVER_SCREENSHOT_H

#include "webserver/bridge.h"

#include "misc/std_filesystem.h"

#include "http/http.h"

namespace Webserver {

// Requests a rendered screenshot and predicts its eventual filename, both
// atomically within one Execute() on the emulation thread (so nothing
// else can request a capture in between and desync the prediction). The
// PNG itself is written asynchronously by a background saver thread --
// Get() polls for it after this command completes.
class ScreenshotCommand : public Command {
public:
	void Execute() override;
	static void Get(const httplib::Request&, httplib::Response&);

private:
	std_fs::path path = {};
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_SCREENSHOT_H
