#include "fgpl/cached_load.h"
#include "check.h"
#include "python_support.h"
#include "wat_module.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace fgpl;
namespace fs = std::filesystem;
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

[[nodiscard]] double Since(std::chrono::steady_clock::time_point begin) {
	return std::chrono::duration<double>(
		std::chrono::steady_clock::now() - begin).count();
}

[[nodiscard]] HostConfig Config(std::vector<std::string> extra = {}) {
	auto config = HostConfig();
	config.path = FGPL_HOST_BINARY;
	config.extraArguments = std::move(extra);
	return config;
}

[[nodiscard]] CacheSettings Settings(const char *name) {
	auto settings = CacheSettings();
	settings.directory = fs::temp_directory_path() / name;
	fs::remove_all(settings.directory);
	settings.secret.fill(0x42);
	return settings;
}

struct Run {
	bool ok = false;
	CachedLoadResult result;
	double seconds = 0;
};

[[nodiscard]] Run LoadAndCheck(
		const HostConfig &config,
		const LoadRequest &request,
		const CacheSettings &settings,
		const cbor::Value &event,
		int32_t expectedStatus) {
	auto run = Run();
	auto started = HostClient::Start(config);
	CHECK(started.ok());
	if (!started.ok()) {
		return run;
	}
	auto &client = *started.value();
	const auto begin = std::chrono::steady_clock::now();
	const auto loaded = LoadWithCache(client, config, request, settings);
	run.seconds = Since(begin);
	if (!loaded.ok()) {
		std::fprintf(stderr, "load: %s\n", loaded.error().detail.c_str());
		CHECK(false);
		return run;
	}
	run.result = loaded.value();
	auto bridge = TestBridge();
	const auto init = client.init(event, bridge);
	CHECK(init.ok());
	const auto answer = client.deliver(event, bridge);
	CHECK(answer.ok() && answer.value() == expectedStatus);
	run.ok = init.ok() && answer.ok() && answer.value() == expectedStatus;
	return run;
}

void TestModuleCacheLifecycle() {
	const auto settings = Settings("fgpl-compile-wat");
	auto request = LoadRequest();
	request.wasm = Wasm(Module("", R"(
		(drop (call $call (local.get $ptr) (local.get $len)
			(i32.const 16) (i32.const 20)))
		(i32.load (i32.const 20)))"));
	const auto event = cbor::Value::FromMap({ { "t", cbor::Value::FromInt(1) } });

	const auto first = LoadAndCheck(Config(), request, settings, event, 5);
	CHECK(first.ok && first.result.source == ModuleSource::Compiled);
	const auto second = LoadAndCheck(Config(), request, settings, event, 5);
	CHECK(second.ok && second.result.source == ModuleSource::Cache);

	const auto cache = ModuleCache(settings.directory, [&] {
		auto key = CacheKey();
		key.secret = settings.secret;
		return key;
	}());
	auto count = 0;
	auto path = fs::path();
	for (const auto &entry : fs::directory_iterator(settings.directory)) {
		if (entry.path().extension() == ".cwasm") {
			++count;
			path = entry.path();
		}
	}
	CHECK(count == 1);

	{
		auto stream = std::fstream(path, std::ios::in | std::ios::out | std::ios::binary);
		stream.seekp(200);
		stream.put('X');
	}
	const auto tampered = LoadAndCheck(Config(), request, settings, event, 5);
	CHECK(tampered.ok && tampered.result.source == ModuleSource::Compiled);

	const auto winch = LoadAndCheck(Config({ "--winch" }), request, settings, event, 5);
	CHECK(winch.ok && winch.result.source == ModuleSource::Compiled);
	const auto winchAgain = LoadAndCheck(Config({ "--winch" }), request, settings, event, 5);
	CHECK(winchAgain.ok && winchAgain.result.source == ModuleSource::Cache);
	fs::remove_all(settings.directory);
}

void TestPoisonedButSignedCacheFallsBack() {
	const auto settings = Settings("fgpl-compile-poison");
	auto request = LoadRequest();
	request.wasm = Wasm(Module("", "(i32.const 7)"));
	const auto event = cbor::Value::FromMap({ { "t", cbor::Value::FromInt(1) } });

	auto started = HostClient::Start(Config());
	CHECK(started.ok());
	if (!started.ok()) {
		return;
	}
	auto key = CacheKey();
	key.secret = settings.secret;
	key.fingerprint = "engine=" + started.value()->sandbox().engine
		+ ";format=1;reservation=16";
	started.value()->kill();
	const auto cache = ModuleCache(settings.directory, std::move(key));
	CHECK(cache.store(request.wasm, std::vector<uint8_t>(5000, 0xCC)));

	const auto run = LoadAndCheck(Config(), request, settings, event, 7);
	CHECK(run.ok);
	CHECK(run.result.cacheRejected);
	CHECK(run.result.source == ModuleSource::Direct);
	const auto next = LoadAndCheck(Config(), request, settings, event, 7);
	CHECK(next.ok && next.result.source == ModuleSource::Compiled);
	fs::remove_all(settings.directory);
}

void TestBadModulesNeverReachTheCache() {
	const auto settings = Settings("fgpl-compile-bad");
	auto request = LoadRequest();
	request.wasm = { 1, 2, 3, 4 };
	auto started = HostClient::Start(Config());
	CHECK(started.ok());
	if (!started.ok()) {
		return;
	}
	const auto loaded = LoadWithCache(
		*started.value(),
		Config(),
		request,
		settings);
	CHECK(!loaded.ok() && loaded.error().error == ClientError::LoadFailed);
	CHECK(!fs::exists(settings.directory)
		|| fs::is_empty(settings.directory));
	fs::remove_all(settings.directory);
}

void TestPythonIsMuchFasterFromTheCache() {
	if (!pysupport::Installed()) {
		return;
	}
	const auto settings = Settings("fgpl-compile-python");
	auto request = pysupport::PythonRequest();
	request.limits.callTimeoutMs = 20000;
	request.limits.memoryBytes = uint64_t(256) << 20;
	auto config = std::getenv("FGPL_CRANELIFT")
		? Config()
		: Config({ "--winch" });
	config.addressSpaceMb = 2048;
	const auto event = cbor::Value::FromMap({
		{ "type", cbor::Value::FromInt(1) },
		{ "text", cbor::Value::FromText("hello") },
	});

	const auto first = LoadAndCheck(config, request, settings, event, 2);
	const auto second = LoadAndCheck(config, request, settings, event, 2);
	CHECK(first.ok && first.result.source == ModuleSource::Compiled);
	CHECK(second.ok && second.result.source == ModuleSource::Cache);
	std::fprintf(
		stderr,
		"python load: first %.2f s (compile + load), from the cache %.2f s\n",
		first.seconds,
		second.seconds);
	CHECK(second.seconds < first.seconds / 2);
	fs::remove_all(settings.directory);
}

} // namespace

int main() {
	TestModuleCacheLifecycle();
	TestPoisonedButSignedCacheFallsBack();
	TestBadModulesNeverReachTheCache();
	TestPythonIsMuchFasterFromTheCache();
	return Failures() ? 1 : 0;
}
