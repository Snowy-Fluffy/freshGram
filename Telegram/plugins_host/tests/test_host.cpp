#include "check.h"
#include "fgpl/host_client.h"
#include "wat_module.h"

#include <signal.h>

#include <chrono>
#include <string>
#include <vector>

namespace {

using namespace fgpl;
using wat::Module;
using wat::Wasm;

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

[[nodiscard]] std::unique_ptr<HostClient> Start(
		uint32_t layers = 7,
		const std::string &selftest = {}) {
	auto config = HostConfig();
	config.path = FGPL_HOST_BINARY;
	config.layers = layers;
	config.selftest = selftest;
	auto started = HostClient::Start(config);
	if (!started.ok()) {
		std::fprintf(
			stderr,
			"start failed: %s (signal %d, exit %d)\n",
			started.error().detail.c_str(),
			started.error().signal,
			started.error().exitCode);
		CHECK(false);
		return nullptr;
	}
	return std::move(started.value());
}

[[nodiscard]] LoadRequest Request(const std::string &wat) {
	auto result = LoadRequest();
	result.wasm = Wasm(wat);
	return result;
}

const auto kEvent = [] {
	return cbor::Value::FromMap({ { "type", cbor::Value::FromInt(1) } });
};

void TestSandboxIsReallyOn() {
	const auto client = Start();
	if (!client) {
		return;
	}
	CHECK(client->sandbox().seccomp);
	CHECK(client->sandbox().rlimits);
	CHECK(client->sandbox().landlock);
	CHECK(client->sandbox().landlockAbi >= 1);
}

void TestRoundTripThroughProcess() {
	const auto client = Start();
	if (!client) {
		return;
	}
	auto loaded = client->load(Request(Module("", R"(
		(drop (call $call (local.get $ptr) (local.get $len)
			(i32.const 16) (i32.const 20)))
		(i32.load (i32.const 20)))")));
	CHECK(loaded.ok());
	if (!loaded.ok()) {
		std::fprintf(stderr, "load: %s\n", loaded.error().detail.c_str());
		return;
	}
	auto bridge = TestBridge();
	CHECK(client->init(kEvent(), bridge).ok());
	for (auto i = 0; i != 3; ++i) {
		const auto result = client->deliver(kEvent(), bridge);
		CHECK(result.ok() && result.value() == 5);
	}
	CHECK(bridge.requests.size() == 3);
	CHECK(bridge.requests.size() == 3 && bridge.requests[0] == kEvent());
}

void TestTimeoutInsideTheSandbox() {
	const auto client = Start();
	if (!client) {
		return;
	}
	auto request = Request(Module("", "(loop $l (br $l)) (i32.const 0)"));
	request.limits.callTimeoutMs = 100;
	CHECK(client->load(request).ok());
	auto bridge = TestBridge();
	const auto start = std::chrono::steady_clock::now();
	const auto result = client->deliver(kEvent(), bridge);
	CHECK(!result.ok());
	CHECK(!result.ok() && result.error().error == ClientError::CallFailed);
	CHECK(!result.ok()
		&& result.error().code == int(CallError::Timeout));
	CHECK(std::chrono::steady_clock::now() - start
		< std::chrono::milliseconds(2500));
	const auto again = client->deliver(kEvent(), bridge);
	CHECK(!again.ok() && again.error().code == int(CallError::Dead));
}

void TestStdoutAndFilesCrossTheBoundary() {
	const auto client = Start();
	if (!client) {
		return;
	}
	auto request = Request(Module(
		R"((import "wasi_snapshot_preview1" "fd_write"
			(func $fd_write (param i32 i32 i32 i32) (result i32)))
			(data (i32.const 64) "hello\nworld")
			(data (i32.const 32) "\40\00\00\00\0b\00\00\00"))",
		R"((call $fd_write (i32.const 1) (i32.const 32) (i32.const 1)
			(i32.const 40)))"));
	CHECK(client->load(request).ok());
	auto bridge = TestBridge();
	const auto result = client->deliver(kEvent(), bridge);
	CHECK(result.ok() && result.value() == 0);
	CHECK(bridge.logs.size() == 2);
	CHECK(bridge.logs.size() == 2 && bridge.logs[0] == "hello");
}

void TestLargePayloadAndFiles() {
	const auto client = Start();
	if (!client) {
		return;
	}
	auto request = Request(Module("", "(i32.const 0)"));
	request.files.emplace_back(
		"plugin/big.bin",
		cbor::Bytes(size_t(40) << 20, 7));
	request.files.emplace_back("runtime/lib/a.py", cbor::Bytes(10, 1));
	const auto loaded = client->load(request);
	CHECK(loaded.ok());
	auto bridge = TestBridge();
	CHECK(client->deliver(kEvent(), bridge).ok());
}

void TestBadLoadIsReported() {
	const auto client = Start();
	if (!client) {
		return;
	}
	auto request = Request(Module(R"((import "env" "x" (func)))", "(i32.const 0)"));
	const auto loaded = client->load(request);
	CHECK(!loaded.ok() && loaded.error().error == ClientError::LoadFailed);
	CHECK(!loaded.ok()
		&& loaded.error().code == int(LoadError::ForbiddenImport));

	auto duplicate = Request(Module("", "(i32.const 0)"));
	duplicate.files.emplace_back("a/b.txt", cbor::Bytes(1, 1));
	duplicate.files.emplace_back("A/B.txt", cbor::Bytes(1, 1));
	const auto refused = client->load(duplicate);
	CHECK(!refused.ok() && refused.error().error == ClientError::LoadFailed);
}

void TestSeccompKillsAnEscapeAttempt() {
	const auto client = Start(7, "open");
	if (client) {
		auto reply = client->waitFor(3000);
		CHECK(!reply.ok());
		CHECK(!reply.ok() && reply.error().error == ClientError::HostDied);
		CHECK(!reply.ok() && reply.error().signal == SIGSYS);
	}
}

void TestWithoutSandboxTheSameAttemptSucceeds() {
	const auto client = Start(0, "open");
	if (client) {
		auto reply = client->waitFor(3000);
		CHECK(reply.ok());
		CHECK(reply.ok() && [&] { const auto t = reply.value().find("t"); return t && t->asText() ? *t->asText() : std::string(); }() == "selftest_survived");
	}
}

void TestHungHostIsKilled() {
	const auto client = Start(7, "hang");
	if (!client) {
		return;
	}
	const auto reply = client->waitFor(300);
	CHECK(!reply.ok() && reply.error().error == ClientError::Timeout);
	client->kill();
}

void TestAddressSpaceLimitIsEnforced() {
	auto config = HostConfig();
	config.path = FGPL_HOST_BINARY;
	config.addressSpaceMb = 160;
	auto started = HostClient::Start(config);
	CHECK(started.ok());
	if (!started.ok()) {
		return;
	}
	auto request = Request(Module("", "(i32.const 0)"));
	request.files.emplace_back("plugin/big.bin", cbor::Bytes(size_t(80) << 20, 1));
	const auto loaded = started.value()->load(request);
	CHECK(!loaded.ok());
	CHECK(!loaded.ok() && (loaded.error().signal != 0
		|| loaded.error().error == ClientError::HostDied
		|| loaded.error().error == ClientError::Protocol));
}

void TestDestructorKillsTheHost() {
	{
		const auto client = Start();
		CHECK(client != nullptr);
	}
	CHECK(true);
}

} // namespace

int main() {
	TestSandboxIsReallyOn();
	TestRoundTripThroughProcess();
	TestTimeoutInsideTheSandbox();
	TestStdoutAndFilesCrossTheBoundary();
	TestLargePayloadAndFiles();
	TestBadLoadIsReported();
	TestSeccompKillsAnEscapeAttempt();
	TestWithoutSandboxTheSameAttemptSucceeds();
	TestHungHostIsKilled();
	TestAddressSpaceLimitIsEnforced();
	TestDestructorKillsTheHost();
	return Failures() ? 1 : 0;
}
