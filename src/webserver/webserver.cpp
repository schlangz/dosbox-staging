// SPDX-FileCopyrightText:  2026-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "webserver.h"
#include "bridge.h"
#include "private/cpu.h"
#include "private/debugger.h"
#include "private/dos.h"
#include "private/dosbox.h"
#include "private/keyboard.h"
#include "private/memory.h"
#include "private/mouse.h"
#include "private/screenshot.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <thread>

#include "http/http.h"
#include "json/json.h"

#include "config/config.h"
#include "dosbox.h"
#include "misc/cross.h"
#include "misc/logging.h"
#include "misc/support.h"

using json = nlohmann::json;

namespace Webserver {

void send_json(httplib::Response& res, const nlohmann::json& j)
{
	res.set_content(j.dump(2), "application/json");
}

void send_error(httplib::Response& res, const int status, const std::string& msg)
{
	json j;
	j["error"] = msg;
	res.status = status;

	send_json(res, j);
}

std::string media_type_of(const std::string& content_type)
{
	auto type = content_type.substr(0, content_type.find(';'));

	const auto first = type.find_first_not_of(" \t");
	if (first == std::string::npos) {
		return {};
	}
	const auto last = type.find_last_not_of(" \t");
	type            = type.substr(first, last - first + 1);

	std::transform(type.begin(), type.end(), type.begin(), [](const char c) {
		return static_cast<char>(
		        std::tolower(static_cast<unsigned char>(c)));
	});
	return type;
}

static void error_handler(const httplib::Request&, httplib::Response& res,
                          std::exception_ptr ep)
{
	std::string msg;
	int status = httplib::StatusCode::InternalServerError_500;

	// The status has to tell a caller what to do next: retry the same
	// request (the emulator was busy), fix the request, or neither.
	try {
		if (ep) {
			std::rethrow_exception(ep);
		}
	} catch (const TimeoutError& e) {
		msg    = e.what();
		status = httplib::StatusCode::GatewayTimeout_504;
	} catch (const std::invalid_argument& e) {
		msg    = e.what();
		status = httplib::StatusCode::BadRequest_400;
	} catch (const nlohmann::json::exception& e) {
		msg    = e.what();
		status = httplib::StatusCode::BadRequest_400;
	} catch (const std::out_of_range& e) {
		msg    = e.what();
		status = httplib::StatusCode::BadRequest_400;
	} catch (const std::exception& e) {
		msg = e.what();
	} catch (...) {
		msg = "Unknown error";
	}

	send_error(res, status, msg);
}

static httplib::Server server;

static void setup_api_handlers()
{
	server.Get("/api/v1/cpu/state", CpuStateCommand::Get);

	server.Get("/api/v1/dos/internals", DosInternalsCommand::Get);

	server.Post("/api/v1/dosbox/shutdown", ShutdownCommand::Post);

	server.Post("/api/v1/memory/allocate", AllocMemoryCommand::Post);
	server.Post("/api/v1/memory/free", FreeMemoryCommand::Post);
	server.Get("/api/v1/memory/:offset/:len", ReadMemoryCommand::Get);
	server.Get("/api/v1/memory/:segment/:offset/:len", ReadMemoryCommand::Get);
	server.Put("/api/v1/memory/:offset", WriteMemoryCommand::Put);
	server.Put("/api/v1/memory/:segment/:offset", WriteMemoryCommand::Put);

	server.Get("/api/v1/debugger/status", DebuggerStatusCommand::Get);
	server.Post("/api/v1/debugger/enable", DebuggerEnableCommand::Post);
	server.Post("/api/v1/debugger/step", DebuggerStepCommand::Post);
	server.Post("/api/v1/debugger/go", DebuggerGoCommand::Post);
	server.Post("/api/v1/debugger/breakpoint/:segment/:offset",
	           DebuggerAddBreakpointCommand::Post);
	server.Delete("/api/v1/debugger/breakpoint/:segment/:offset",
	             DebuggerDeleteBreakpointCommand::Delete);
	server.Post("/api/v1/debugger/logpoint/:segment/:offset",
	           DebuggerAddLogpointCommand::Post);
	server.Delete("/api/v1/debugger/logpoint/:segment/:offset",
	             DebuggerDeleteLogpointCommand::Delete);
	server.Post("/api/v1/debugger/logsig", DebuggerAddLogsigCommand::Post);
	server.Delete("/api/v1/debugger/logsig", DebuggerDeleteLogsigCommand::Delete);
	server.Get("/api/v1/debugger/callrec", CallrecStatusCommand::Get);
	server.Post("/api/v1/debugger/callrec", CallrecSetCommand::Post);
	server.Post("/api/v1/debugger/callrec/dump", CallrecDumpCommand::Post);
	server.Post("/api/v1/debugger/command", DebuggerCommandCommand::Post);
	server.Post("/api/v1/debugger/re_dump_toggle", ReDumpToggleCommand::Post);

	server.Post("/api/v1/keyboard/key", KeyboardKeyCommand::Post);
	server.Post("/api/v1/keyboard/type", KeyboardTypeCommand::Post);

	server.Get("/api/v1/mouse/status", MouseStatusCommand::Get);
	server.Post("/api/v1/mouse/move", MouseMoveCommand::Post);
	server.Post("/api/v1/mouse/button", MouseButtonCommand::Post);

	server.Get("/api/v1/screenshot", ScreenshotCommand::Get);
}

static std::string strip_port(const std::string& host)
{
	// IPv6 literal: [::1]:8080
	if (host.size() > 1 && host[0] == '[') {
		const auto bracket = host.rfind(']');
		if (bracket != std::string::npos) {
			return host.substr(0, bracket + 1);
		}
		return host;
	}

	// IPv4 or hostname: 127.0.0.1:8080
	const auto colon = host.rfind(':');
	if (colon != std::string::npos) {
		return host.substr(0, colon);
	}
	return host;
}

// Whether the host part is written as a literal IP address rather than a name.
//
// This is what the Host check is really about. DNS rebinding works by making a
// *name* the browser already trusts resolve to an address the attacker did not
// have access to; an address literal has no resolution step to subvert, so it
// cannot be rebound. Accepting literals therefore costs nothing and is what
// lets a wildcard bind be reached over the machine's own interface addresses.
static bool is_ip_literal(const std::string& host)
{
	if (host.empty()) {
		return false;
	}

	// IPv6 literals arrive bracketed, and the brackets are the only place
	// ':' may appear in a Host value once the port has been stripped.
	if (host.front() == '[' && host.back() == ']') {
		const auto inner = host.substr(1, host.size() - 2);
		return !inner.empty() &&
		       inner.find_first_not_of("0123456789abcdefABCDEF:.") ==
		               std::string::npos;
	}

	// IPv4 dotted quad: digits and dots only, and at least one dot, so a
	// bare name of digits is not mistaken for one.
	return host.find_first_not_of("0123456789.") == std::string::npos &&
	       host.find('.') != std::string::npos;
}

static void setup_host_validation(const std::string& addr, int port)
{
	// Build the set of allowed Host header values to prevent DNS
	// rebinding attacks. A rebound domain would not match any of these.
	std::set<std::string> allowed;

	const auto port_str = ":" + std::to_string(port);

	auto add = [&](const std::string& hostname) {
		allowed.emplace(hostname);
		allowed.emplace(hostname + port_str);
	};

	add(addr);

	if (addr == "127.0.0.1" || addr == "0.0.0.0") {
		add("localhost");
	}
	if (addr == "::1" || addr == "::") {
		add("localhost");
		add("[::1]");
	}

	// A wildcard bind is reachable on every interface the host has, so the
	// Host value a client sends is whichever of those addresses it used.
	// Enumerating them would still miss a later DHCP change, so accept any
	// address literal instead; see is_ip_literal().
	const bool wildcard_bind = (addr == "0.0.0.0" || addr == "::");

	server.set_pre_routing_handler(
	        [allowed = std::move(allowed),
	         wildcard_bind](const httplib::Request& req, httplib::Response& res) {
		        const auto host = strip_port(req.get_header_value("Host"));

		        const bool ok = (allowed.find(host) != allowed.end()) ||
		                        (wildcard_bind && is_ip_literal(host));

		        if (!ok) {
			        LOG_WARNING("WEBSERVER: Rejected request with Host header '%s'",
			                    req.get_header_value("Host").c_str());

			        res.status = httplib::StatusCode::Forbidden_403;
			        res.set_content("Forbidden", "text/plain");

			        return httplib::Server::HandlerResponse::Handled;
		        }
		        return httplib::Server::HandlerResponse::Unhandled;
	        });
}

static void run(const std::string addr, const int port, const std::string resource_home)
{
	const auto config_home = (get_config_dir() / DefaultWebserverDir).string();

	server.set_mount_point("/", config_home);
	server.set_mount_point("/", resource_home);

	setup_api_handlers();
	setup_host_validation(addr, port);

	server.set_exception_handler(error_handler);

	server.Get("/api/v1/dosbox/info", [=](auto, auto& res) {
		json j;
		j["configHome"]      = get_config_dir();
		j["configWebserver"] = config_home;
		j["version"]         = DOSBOX_GetDetailedVersion();

		send_json(res, j);
	});

	LOG_INFO("WEBSERVER: Starting HTTP REST API on http://%s:%d",
	         addr.c_str(),
	         port);

	LOG_INFO("WEBSERVER: Using document root directory '%s'",
	         config_home.c_str());

	auto ok = server.listen(addr, port);
	if (!ok) {
		LOG_WARNING("WEBSERVER: Failed to bind to %s:%d", addr.c_str(), port);
	}
}

static void init_config_settings(SectionProp& section)
{
	using enum Property::Changeable::Value;

	auto enabled = section.AddBool("webserver_enabled", OnlyAtStart, false);
	enabled->SetHelp(
	        "Enable the HTTP REST API that exposes internal state and memory (disabled by\n"
	        "default). Open http://localhost:8086 in a browser (or use the configured port)\n"
	        "to view the API documentation.");
	auto bind_ip = section.AddString("webserver_bind_address",
	                                 OnlyAtStart,
	                                 "127.0.0.1");
	bind_ip->SetHelp(
	        "Bind to the given IP address. This API gives full control over DOSBox, do not\n"
	        "ever expose this to untrusted hosts.\n"
	        "\n"
	        "By default only local connections are allowed.");

	auto bind_port = section.AddInt("webserver_port", OnlyAtStart, 8086);
	bind_port->SetMinMax(1, 0xFFFF);
	bind_port->SetHelp("TCP port to bind to.");
}

} // namespace Webserver

static bool is_webserver_enabled = false;

void WEBSERVER_Init()
{
	auto section = get_section("webserver");

	if (section->GetBool("webserver_enabled")) {
		is_webserver_enabled = true;

		const auto addr = section->GetString("webserver_bind_address");
		const auto port = section->GetInt("webserver_port");
		const auto resource_home = get_resource_path("webserver").string();

		std::thread thread(Webserver::run, addr, port, resource_home);

		thread.detach();
	}
}

void WEBSERVER_Destroy()
{
	Webserver::server.stop();
}

void WEBSERVER_AddConfigSection(const ConfigPtr& conf)
{
	assert(conf);

	auto section = conf->AddSection("webserver");

	Webserver::init_config_settings(*section);
}

bool WEBSERVER_IsEnabled()
{
	return is_webserver_enabled;
}
