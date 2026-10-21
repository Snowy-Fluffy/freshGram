#include "check.h"
#include "fgpl/host_client.h"
#include "python_support.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace fgpl;
using pysupport::PythonRequest;

class TestBridge final : public Bridge {
public:
	cbor::Value call(const cbor::Value &request) override {
		requests.push_back(request);
		return cbor::Value::FromMap({
			{ "ok", cbor::Value::FromBool(true) },
		});
	}
	void log(std::string_view line) override {
		logs.emplace_back(line);
	}

	std::vector<cbor::Value> requests;
	std::vector<std::string> logs;

};

[[nodiscard]] double Seconds(std::chrono::steady_clock::time_point since) {
	return std::chrono::duration<double>(
		std::chrono::steady_clock::now() - since).count();
}

void RunWith(const char *label, const std::vector<std::string> &extra) {
	auto config = HostConfig();
	config.path = FGPL_HOST_BINARY;
	config.addressSpaceMb = std::getenv("FGPL_AS_MB") ? std::atoi(std::getenv("FGPL_AS_MB")) : 4096;
	config.extraArguments = extra;
	auto started = HostClient::Start(config);
	CHECK(started.ok());
	if (!started.ok()) {
		return;
	}
	auto &client = started.value();
	CHECK(client->sandbox().seccomp);

	const auto request = PythonRequest();
	const auto loadStart = std::chrono::steady_clock::now();
	const auto loaded = client->load(request);
	CHECK(loaded.ok());
	if (!loaded.ok()) {
		std::fprintf(stderr, "load: %s\n", loaded.error().detail.c_str());
		return;
	}
	const auto loadSeconds = Seconds(loadStart);

	auto bridge = TestBridge();
	const auto firstStart = std::chrono::steady_clock::now();
	const auto init = client->init(
		cbor::Value::FromMap({ { "plugin", cbor::Value::FromText("test") } }),
		bridge);
	if (!init.ok()) {
		std::fprintf(
			stderr,
			"init: %s (signal %d, exit %d, code %d)\n",
			init.error().detail.c_str(),
			init.error().signal,
			init.error().exitCode,
			init.error().code);
		for (const auto &line : bridge.logs) {
			std::fprintf(stderr, "  log: %s\n", line.c_str());
		}
	}
	CHECK(init.ok());
	const auto firstSeconds = Seconds(firstStart);

	const auto eventStart = std::chrono::steady_clock::now();
	auto event = cbor::Value::FromMap({
		{ "type", cbor::Value::FromInt(1) },
		{ "text", cbor::Value::FromText("hello") },
	});
	for (auto i = 0; i != 20; ++i) {
		const auto result = client->deliver(event, bridge);
		CHECK(result.ok() && result.value() == 2);
	}
	const auto eventSeconds = Seconds(eventStart) / 20;
	std::fprintf(
		stderr,
		"%s: load %.2f s, python start + init %.2f s, event %.1f ms\n",
		label,
		loadSeconds,
		firstSeconds,
		eventSeconds * 1000);

	CHECK(bridge.requests.size() == 21);
	CHECK(bridge.requests.size() == 21
		&& bridge.requests[1].find("op")
		&& *bridge.requests[1].find("op")->asText() == "echo");
	auto found = false;
	for (const auto &line : bridge.logs) {
		found = found || line.find("response: {'ok': True}") != std::string::npos;
	}
	CHECK(found);

	auto crash = cbor::Value::FromMap({ { "crash", cbor::Value::FromBool(true) } });
	const auto failed = client->deliver(crash, bridge);
	CHECK(!failed.ok());
	CHECK(!failed.ok() && failed.error().error == ClientError::CallFailed);
}

} // namespace

int main() {
	if (!pysupport::Installed()) {
		std::fprintf(stderr, "python.wasm is not installed, skipping\n");
		return 0;
	}
	RunWith("cranelift", {});
	RunWith("winch", { "--winch" });
	return Failures() ? 1 : 0;
}
