#include "fgpl/broker.h"
#include "check.h"
#include "fakes.h"
#include "fgpl/host_client.h"
#include "python_support.h"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace fgpl;

[[nodiscard]] cbor::Value Event(int64_t chat) {
	return cbor::Value::FromMap({
		{ "type", cbor::Value::FromText("message.new") },
		{ "chat", cbor::Value::FromInt(chat) },
		{ "text", cbor::Value::FromText("hello") },
	});
}

struct World {
	explicit World(std::set<std::string> permissions) {
		grants.pluginId = "com.example.test";
		grants.permissions = std::move(permissions);
		grants.storageQuotaBytes = 4096;
		services.storage = &storage;
		services.data = &data;
		services.transport = &transport;
		services.log = [this](std::string_view line) {
			logs.emplace_back(line);
		};
	}

	[[nodiscard]] std::optional<cbor::Value> stored(const std::string &key) {
		const auto bytes = storage.get(key);
		if (!bytes) {
			return std::nullopt;
		}
		auto decoded = cbor::Decode(*bytes);
		return decoded.ok()
			? std::optional<cbor::Value>(std::move(decoded.value()))
			: std::nullopt;
	}

	Grants grants;
	BrokerServices services;
	MemoryStorage storage;
	fakes::FakeData data;
	fakes::FakeTransport transport;
	std::vector<std::string> logs;

};

[[nodiscard]] std::unique_ptr<HostClient> Start(
		std::vector<std::string> extra = {}) {
	auto config = HostConfig();
	config.path = FGPL_HOST_BINARY;
	config.extraArguments = std::move(extra);
	config.addressSpaceMb = 2048;
	auto started = HostClient::Start(config);
	CHECK(started.ok());
	return started.ok() ? std::move(started.value()) : nullptr;
}

[[nodiscard]] std::string StringAt(const cbor::Value &report, const char *key) {
	const auto found = report.find(key);
	return (found && found->asText()) ? *found->asText() : std::string("<none>");
}

void TestPythonSdk() {
	if (!pysupport::Installed()) {
		return;
	}
	auto world = World({
		"storage.private", "net.https", "chats.list", "account.info",
		"messages.history", "stars.balance", "ui.toast", "messages.read",
	});
	auto broker = Broker(world.grants, world.services);
	auto client = Start({ "--winch" });
	if (!client) {
		return;
	}
	auto request = pysupport::PythonRequest(FGPL_PYTHON_PLUGIN_DIR, "sdk_demo.py");
	request.limits.callTimeoutMs = 20000;
	request.limits.memoryBytes = uint64_t(256) << 20;
	const auto loaded = client->load(request);
	CHECK(loaded.ok());
	if (!loaded.ok()) {
		return;
	}
	CHECK(client->init(cbor::Value::FromMap({}), broker).ok());
	const auto event = broker.filterEvent(Event(1));
	CHECK(event.has_value());
	const auto result = client->deliver(*event, broker);
	if (!result.ok()) {
		std::fprintf(stderr, "deliver: %s\n", result.error().detail.c_str());
		for (const auto &line : world.logs) {
			std::fprintf(stderr, "  log: %s\n", line.c_str());
		}
	}
	CHECK(result.ok() && result.value() == 0);

	const auto started = world.stored("started");
	CHECK(started && started->asBool() == true);
	const auto report = world.stored("report");
	CHECK(report.has_value());
	if (!report) {
		for (const auto &line : world.logs) {
			std::fprintf(stderr, "  log: %s\n", line.c_str());
		}
		return;
	}
	CHECK(StringAt(*report, "plugin") == "com.example.test");
	CHECK(report->find("count")->asInt() == 1);
	CHECK(report->find("granted")->asArray()->size() == 8);
	const auto chats = report->find("chats")->asArray();
	CHECK(chats && chats->size() == 3);
	CHECK(chats && chats->size() == 3 && (*chats)[0].asInt() == 1);
	CHECK(report->find("has_phone")->asBool() == false);
	CHECK(StringAt(*report, "service_chat") == "denied");
	CHECK(StringAt(*report, "secret_chat") == "not_found");
	CHECK(report->find("history")->asInt() == 3);
	const auto mine = report->find("mine")->asArray();
	CHECK(mine && mine->size() == 2 && (*mine)[0].asBool() == true);
	CHECK(report->find("server_time")->asInt() == 1700000000);
	const auto got = report->find("got")->asArray();
	CHECK(got && got->size() == 3 && (*got)[0].asInt() == 100 && (*got)[1].asInt() == 1 && (*got)[2].asInt() == 2);
	const auto fetched = report->find("fetch")->asArray();
	CHECK(fetched && fetched->size() == 2);
	CHECK(fetched && (*fetched)[0].asInt() == 200);
	CHECK(fetched && *(*fetched)[1].asText() == "xxxxx");
	CHECK(StringAt(*report, "other_host") == "ok");
	CHECK(StringAt(*report, "public_ip") == "ok");
	CHECK(StringAt(*report, "tcp") == "denied");
	CHECK(StringAt(*report, "http_url") == "invalid");
	CHECK(StringAt(*report, "ip_url") == "invalid");
	CHECK(StringAt(*report, "cookie_header") == "invalid");
	CHECK(StringAt(*report, "quota") == "quota");
	CHECK(report->find("stars")->asInt() == 1234);
	CHECK(world.data.toasts.size() == 1);
	CHECK(world.transport.requests.size() == 3);
	CHECK(world.transport.requests[0].headers.size() == 1);

	const auto second = client->deliver(*broker.filterEvent(Event(2)), broker);
	CHECK(second.ok());
	CHECK(world.stored("report")->find("count")->asInt() == 2);
}

void TestPythonSdkWithoutPermissions() {
	if (!pysupport::Installed()) {
		return;
	}
	auto world = World({ "messages.read" });
	auto broker = Broker(world.grants, world.services);
	auto client = Start({ "--winch" });
	if (!client) {
		return;
	}
	auto request = pysupport::PythonRequest(FGPL_PYTHON_PLUGIN_DIR, "denied_demo.py");
	request.limits.callTimeoutMs = 20000;
	request.limits.memoryBytes = uint64_t(256) << 20;
	CHECK(client->load(request).ok());
	CHECK(client->deliver(*broker.filterEvent(Event(1)), broker).ok());
	auto joined = std::string();
	for (const auto &line : world.logs) {
		joined += line;
	}
	for (const auto name : { "storage", "net", "chats", "account", "stars", "toast", "send" }) {
		const auto expected = std::string("('") + name + "', 'denied')";
		if (joined.find(expected) == std::string::npos) {
			std::fprintf(stderr, "%s was not denied: %s\n", name, joined.c_str());
			CHECK(false);
		}
	}
	CHECK(world.storage.count() == 0);
	CHECK(world.transport.requests.empty());
	CHECK(world.data.sent.empty());
}

#ifdef FGPL_COUNTER_WASM

[[nodiscard]] cbor::Bytes ReadFile(const char *path) {
	auto stream = std::ifstream(path, std::ios::binary);
	return { std::istreambuf_iterator<char>(stream), {} };
}

void TestCSdk() {
	auto world = World({ "storage.private", "messages.read" });
	auto broker = Broker(world.grants, world.services);
	auto client = Start();
	if (!client) {
		return;
	}
	auto request = LoadRequest();
	request.wasm = ReadFile(FGPL_COUNTER_WASM);
	CHECK(!request.wasm.empty());
	CHECK(client->load(request).ok());
	CHECK(client->init(cbor::Value::FromMap({}), broker).ok());
	for (auto i = 1; i <= 5; ++i) {
		const auto result = client->deliver(*broker.filterEvent(Event(7)), broker);
		CHECK(result.ok() && result.value() == i);
	}
	const auto count = world.stored("count");
	CHECK(count && count->asInt() == 5);
	const auto chat = world.stored("last_chat");
	CHECK(chat && chat->asInt() == 7);
	CHECK(!world.logs.empty() && world.logs.front() == "counter ready");
}

void TestCSdkDenied() {
	auto world = World({ "messages.read" });
	auto broker = Broker(world.grants, world.services);
	auto client = Start();
	if (!client) {
		return;
	}
	auto request = LoadRequest();
	request.wasm = ReadFile(FGPL_COUNTER_WASM);
	CHECK(client->load(request).ok());
	const auto result = client->deliver(*broker.filterEvent(Event(7)), broker);
	CHECK(result.ok() && result.value() == 100 + 1);
	CHECK(world.storage.count() == 0);
	auto saw = false;
	for (const auto &line : world.logs) {
		saw = saw || line == "cannot read the counter";
	}
	CHECK(saw);
}

#endif

} // namespace

int main() {
	TestPythonSdk();
	TestPythonSdkWithoutPermissions();
#ifdef FGPL_COUNTER_WASM
	TestCSdk();
	TestCSdkDenied();
#endif
	return Failures() ? 1 : 0;
}
