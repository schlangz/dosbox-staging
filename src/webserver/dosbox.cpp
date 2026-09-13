// SPDX-FileCopyrightText:  2026-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "private/dosbox.h"

#include "webserver.h"

#include "dosbox.h"
#include "json/json.h"

namespace Webserver {

void ShutdownCommand::Execute()
{
	DOSBOX_RequestShutdown();
}

void ShutdownCommand::Post(const httplib::Request&, httplib::Response& res)
{
	ShutdownCommand cmd;
	cmd.WaitForCompletion();

	if (!cmd.error.empty()) {
		throw std::runtime_error(cmd.error);
	}

	nlohmann::json out;
	out["shutdown_requested"] = true;
	send_json(res, out);
}

} // namespace Webserver
