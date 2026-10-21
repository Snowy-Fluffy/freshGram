#include "check.h"
#include "host_client.h"
#include "python_support.h"
#include "wat_module.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace fgpl;
using wat::Module;
using wat::Wasm;

class AbuseBridge final : public Bridge {
public:
	cbor::Value call(const cbor::Value &request) override {
		requests.push_back(request);
		return cbor::Value::FromMap({
			{ "ok", cbor::Value::FromBool(true) },
		});
	}
	void log(std::string_view line) override {
		logBytes += line.size() + 1;
		logs.emplace_back(line);
	}

	std::vector<cbor::Value> requests;
	std::vector<std::string> logs;
	size_t logBytes = 0;

};

[[nodiscard]] double Since(std::chrono::steady_clock::time_point begin) {
	return std::chrono::duration<double>(
		std::chrono::steady_clock::now() - begin).count();
}

[[nodiscard]] std::unique_ptr<HostClient> Start(
		const std::string &selftest = {},
		int cpuSeconds = 0,
		std::vector<std::string> extra = {}) {
	auto config = HostConfig();
	config.path = FGPL_HOST_BINARY;
	config.selftest = selftest;
	config.cpuSeconds = cpuSeconds;
	config.addressSpaceMb = 2048;
	config.extraArguments = std::move(extra);
	auto started = HostClient::Start(config);
	CHECK(started.ok());
	return started.ok() ? std::move(started.value()) : nullptr;
}

const auto kEvent = [] {
	return cbor::Value::FromMap({ { "type", cbor::Value::FromInt(1) } });
};

void TestGuestCallSpamIsCapped() {
	const auto client = Start();
	if (!client) {
		return;
	}
	auto request = LoadRequest();
	request.wasm = Wasm(Module("", R"(
		(local $i i32) (local $s i32)
		(loop $l
			(local.set $s (call $call (local.get $ptr) (local.get $len)
				(i32.const 16) (i32.const 20)))
			(local.set $i (i32.add (local.get $i) (i32.const 1)))
			(br_if $l (i32.lt_u (local.get $i) (i32.const 100000))))
		(local.get $s))"));
	const auto loadedSpam = client->load(request);
	if (!loadedSpam.ok()) {
		std::fprintf(
			stderr,
			"load failed: %s (error %d, signal %d, exit %d)\n",
			loadedSpam.error().detail.c_str(),
			int(loadedSpam.error().error),
			loadedSpam.error().signal,
			loadedSpam.error().exitCode);
	}
	CHECK(loadedSpam.ok());
	auto bridge = AbuseBridge();
	const auto begin = std::chrono::steady_clock::now();
	const auto result = client->deliver(kEvent(), bridge);
	CHECK(result.ok() && result.value() == guest_status::kTooManyCalls);
	CHECK(bridge.requests.size() == 256);
	CHECK(Since(begin) < 3);
	CHECK(client->deliver(kEvent(), bridge).ok());
	CHECK(bridge.requests.size() == 512);
}

void TestGuestLogSpamIsContained() {
	const auto client = Start();
	if (!client) {
		return;
	}
	auto request = LoadRequest();
	request.wasm = Wasm(Module(
		R"((import "wasi_snapshot_preview1" "fd_write"
			(func $fd_write (param i32 i32 i32 i32) (result i32)))
			(data (i32.const 64) "spam spam spam spam\n")
			(data (i32.const 32) "\40\00\00\00\14\00\00\00"))",
		R"((loop $l
			(drop (call $fd_write (i32.const 1) (i32.const 32) (i32.const 1)
				(i32.const 40)))
			(br $l))
		(i32.const 0))"));
	request.limits.callTimeoutMs = 300;
	CHECK(client->load(request).ok());
	auto bridge = AbuseBridge();
	const auto begin = std::chrono::steady_clock::now();
	const auto result = client->deliver(kEvent(), bridge);
	CHECK(!result.ok());
	CHECK(!result.ok() && result.error().error == ClientError::CallFailed);
	CHECK(!result.ok() && result.error().code == int(CallError::Timeout));
	CHECK(bridge.logBytes <= (size_t(1) << 20));
	CHECK(Since(begin) < 5);
}

void TestMemoryBombInsideTheGuest() {
	const auto client = Start();
	if (!client) {
		return;
	}
	auto request = LoadRequest();
	request.wasm = Wasm(Module("", R"(
		(local $i i32)
		(loop $l
			(if (i32.ne (memory.grow (i32.const 256)) (i32.const -1))
				(then (local.set $i (i32.add (local.get $i) (i32.const 1)))
					(br $l))))
		(local.get $i))"));
	request.limits.memoryBytes = uint64_t(32) << 20;
	CHECK(client->load(request).ok());
	auto bridge = AbuseBridge();
	const auto result = client->deliver(kEvent(), bridge);
	CHECK(result.ok() && result.value() == 1);
	CHECK(client->deliver(kEvent(), bridge).ok());
}

void TestRunawayHostIsStoppedByTheCpuLimit() {
	const auto client = Start("spin", 1);
	if (!client) {
		return;
	}
	const auto begin = std::chrono::steady_clock::now();
	const auto message = client->waitFor(6000);
	CHECK(!message.ok());
	CHECK(!message.ok() && message.error().error == ClientError::HostDied);
	CHECK(!message.ok()
		&& (message.error().signal == SIGXCPU
			|| message.error().signal == SIGKILL));
	CHECK(Since(begin) < 6);
}

[[nodiscard]] bool Gone(int pid) {
	for (auto i = 0; i != 40; ++i) {
		auto stream = std::ifstream("/proc/" + std::to_string(pid) + "/stat");
		auto line = std::string();
		if (!std::getline(stream, line)) {
			return true;
		}
		const auto close = line.rfind(')');
		if (close != std::string::npos
			&& line.size() > close + 2
			&& line[close + 2] == 'Z') {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	return false;
}

void TestHostDiesWithItsParent() {
	int channel[2];
	CHECK(pipe(channel) == 0);
	const auto middle = fork();
	if (middle == 0) {
		close(channel[0]);
		auto config = HostConfig();
		config.path = FGPL_HOST_BINARY;
		auto started = HostClient::Start(config);
		const auto pid = started.ok() ? started.value()->pid() : -1;
		if (write(channel[1], &pid, sizeof(pid)) != sizeof(pid)) {
			_exit(1);
		}
		pause();
		_exit(0);
	}
	close(channel[1]);
	auto hostPid = 0;
	CHECK(read(channel[0], &hostPid, sizeof(hostPid)) == sizeof(hostPid));
	CHECK(hostPid > 0);
	kill(middle, SIGKILL);
	auto status = 0;
	waitpid(middle, &status, 0);
	CHECK(Gone(hostPid));
	close(channel[0]);
}

void TestPython(
		const char *attack,
		bool expectSuccess,
		const std::function<void(AbuseBridge &, const Result<int32_t, ClientFailure> &, double)> &check) {
	auto client = Start("", 0, { "--winch" });
	if (!client) {
		return;
	}
	auto request = pysupport::PythonRequest(
		FGPL_PYTHON_PLUGIN_DIR,
		"attacks.py");
	request.limits.callTimeoutMs = 700;
	request.limits.memoryBytes = uint64_t(256) << 20;
	const auto loaded = client->load(request);
	if (!loaded.ok()) {
		std::fprintf(
			stderr,
			"python load failed: %s (error %d, signal %d, exit %d, code %d)\n",
			loaded.error().detail.c_str(),
			int(loaded.error().error),
			loaded.error().signal,
			loaded.error().exitCode,
			loaded.error().code);
	}
	CHECK(loaded.ok());
	if (!loaded.ok()) {
		return;
	}
	auto bridge = AbuseBridge();
	CHECK(client->init(kEvent(), bridge).ok());
	const auto begin = std::chrono::steady_clock::now();
	const auto result = client->deliver(
		cbor::Value::FromMap({ { "attack", cbor::Value::FromText(attack) } }),
		bridge);
	const auto seconds = Since(begin);
	if (result.ok() != expectSuccess) {
		std::fprintf(
			stderr,
			"python %s: expected %s (%s)\n",
			attack,
			expectSuccess ? "success" : "failure",
			result.ok() ? "" : result.error().detail.c_str());
		CHECK(false);
	}
	check(bridge, result, seconds);
}

void TestPythonAttacks() {
	using Check = const Result<int32_t, ClientFailure> &;
	TestPython("garbage", false, [](AbuseBridge &, Check result, double) {
		CHECK(!result.ok() && result.error().error == ClientError::CallFailed);
	});
	TestPython("oversized", false, [](AbuseBridge &, Check result, double) {
		CHECK(!result.ok() && result.error().error == ClientError::CallFailed);
	});
	TestPython("flood", true, [](AbuseBridge &bridge, Check result, double) {
		CHECK(result.ok() && result.value() == 1000 - 256);
		CHECK(bridge.requests.size() == 256);
	});
	TestPython("spin", false, [](AbuseBridge &, Check result, double seconds) {
		CHECK(!result.ok() && result.error().code == int(CallError::Timeout));
		CHECK(seconds < 5);
	});
	TestPython("memory", true, [](AbuseBridge &, Check result, double) {
		CHECK(result.ok() && result.value() == 1);
	});
	TestPython("exit", false, [](AbuseBridge &, Check result, double) {
		CHECK(!result.ok() && result.error().error == ClientError::CallFailed);
	});
	TestPython("stdout", true, [](AbuseBridge &bridge, Check, double) {
		auto seen = false;
		for (const auto &line : bridge.logs) {
			seen = seen || line.find("goes to the log") != std::string::npos;
		}
		CHECK(seen);
	});
	TestPython("escape", true, [](AbuseBridge &bridge, Check, double) {
		const cbor::Value *report = nullptr;
		for (const auto &request : bridge.requests) {
			if (request.find("escape")) {
				report = request.find("escape");
			}
		}
		CHECK(report != nullptr);
		if (!report || !report->asMap()) {
			return;
		}
		for (const auto &entry : *report->asMap()) {
			if (entry.key == "environ") {
				continue;
			}
			const auto text = entry.value.asText();
			if (!text || text->rfind("error:", 0) != 0) {
				std::fprintf(
					stderr,
					"escape '%s' was not refused: %s\n",
					entry.key.c_str(),
					text ? text->c_str() : "?");
				CHECK(false);
			}
		}
	});
}

} // namespace

int main() {
	TestGuestCallSpamIsCapped();
	TestGuestLogSpamIsContained();
	TestMemoryBombInsideTheGuest();
	TestRunawayHostIsStoppedByTheCpuLimit();
	TestHostDiesWithItsParent();
	if (pysupport::Installed()) {
		TestPythonAttacks();
	}
	return Failures() ? 1 : 0;
}
