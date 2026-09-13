// SPDX-FileCopyrightText:  2026-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_WEBSERVER_SCREENSHOT_H
#define DOSBOX_WEBSERVER_SCREENSHOT_H

#include "webserver/bridge.h"

#include "misc/std_filesystem.h"

#include "http/http.h"

namespace Webserver {

// Requests a rendered screenshot and predicts its eventual filename. The PNG
// itself is written asynchronously by a background saver thread, so Get()
// polls for the file after this command completes.
//
// LIMITATION, prediction race: the capture index the filename is built from is
// only consumed when the capture actually happens, inside the image capturer,
// not when the request is made. Reserving it here would mean the capture
// subsystem handing back the reserved name, which it has no interface for.
// Concurrent screenshot requests over HTTP are serialised in Get() so they
// cannot race each other; a screenshot started from the F5 hotkey in the same
// window still can, in which case this handler waits for, and returns, the
// wrong file or times out.
class ScreenshotCommand : public Command {
public:
	void Execute() override;
	static void Get(const httplib::Request&, httplib::Response&);

private:
	std_fs::path path = {};
};

} // namespace Webserver

#endif // DOSBOX_WEBSERVER_SCREENSHOT_H
