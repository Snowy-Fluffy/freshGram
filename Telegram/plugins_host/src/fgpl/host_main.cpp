#include "fgpl/ipc.h"
#include "fgpl/protocol.h"
#include "fgpl/runtime.h"
#include "fgpl/sandbox_linux.h"

#include <fcntl.h>
#include <malloc.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

extern char **environ;

namespace {

using namespace fgpl;

class SocketBridge final : public Bridge {
public:
	explicit SocketBridge(int fd) : _fd(fd) {
	}

	cbor::Value call(const cbor::Value &request) override {
		if (!ipc::WriteFrame(_fd, proto::Map({
				{ "t", proto::Text("call") },
				{ "v", request },
			})).ok()) {
			std::_Exit(3);
		}
		while (true) {
			auto frame = ipc::ReadFrame(_fd, ipc::TrustedLimits());
			if (!frame.ok()) {
				std::_Exit(3);
			}
			const auto type = proto::TypeOf(frame.value());
			if (type == "reply") {
				const auto value = frame.value().find("v");
				return value ? *value : cbor::Value();
			} else if (type == "stop") {
				std::_Exit(0);
			}
			return proto::Map({ { "error", proto::Text("protocol") } });
		}
	}

	void log(std::string_view line) override {
		if (!ipc::WriteFrame(_fd, proto::Map({
				{ "t", proto::Text("log") },
				{ "line", proto::Text(line) },
			})).ok()) {
			std::_Exit(3);
		}
	}

private:
	int _fd = -1;

};

void Fatal(int fd, const std::string &detail) {
	if (!ipc::WriteFrame(fd, proto::Map({
			{ "t", proto::Text("fatal") },
			{ "detail", proto::Text(detail) },
		})).ok()) {
		std::_Exit(2);
	}
	std::_Exit(2);
}

[[nodiscard]] RuntimeLimits LimitsFrom(const cbor::Value &message) {
	auto result = RuntimeLimits();
	const auto limits = message.find("limits");
	if (!limits) {
		return result;
	}
	const auto memory = proto::IntField(*limits, "memory", 0);
	if (memory > 0) {
		result.memoryBytes = uint64_t(memory);
	}
	const auto timeout = proto::IntField(*limits, "timeout", 0);
	if (timeout > 0) {
		result.callTimeoutMs = uint32_t(timeout);
	}
	const auto start = proto::IntField(*limits, "start", 0);
	if (start > 0) {
		result.startTimeoutMs = uint32_t(start);
	}
	const auto hosted = proto::IntField(*limits, "hostcall", 0);
	if (hosted > 0) {
		result.maxHostCallMs = uint32_t(hosted);
	}
	const auto message_bytes = proto::IntField(*limits, "message", 0);
	if (message_bytes > 0) {
		result.maxMessageBytes = size_t(message_bytes);
	}
	return result;
}

void Reply(int fd, const cbor::Value &message) {
	if (!ipc::WriteFrame(fd, message).ok()) {
		std::_Exit(3);
	}
}

void RunCompiler(int fd, Runtime &runtime) {
	auto frame = ipc::ReadFrame(fd, ipc::TrustedLimits());
	if (!frame.ok() || proto::TypeOf(frame.value()) != "compile") {
		std::_Exit(3);
	}
	const auto wasm = frame.value().find("wasm");
	const auto bytes = wasm ? wasm->asBytes() : nullptr;
	if (!bytes) {
		Reply(fd, proto::Map({
			{ "t", proto::Text("compile_failed") },
			{ "detail", proto::Text("no module") },
		}));
		std::_Exit(0);
	}
	const auto compiled = runtime.compile(*bytes);
	if (!compiled.ok()) {
		Reply(fd, proto::Map({
			{ "t", proto::Text("compile_failed") },
			{ "detail", proto::Text(compiled.error().detail) },
		}));
		std::_Exit(0);
	}
	constexpr auto kChunk = size_t(1) << 20;
	const auto &data = compiled.value();
	const auto count = (data.size() + kChunk - 1) / kChunk;
	for (auto i = size_t(0); i != count; ++i) {
		const auto from = i * kChunk;
		const auto to = std::min(data.size(), from + kChunk);
		Reply(fd, proto::Map({
			{ "t", proto::Text("compiled") },
			{ "i", proto::Int(int64_t(i)) },
			{ "n", proto::Int(int64_t(count)) },
			{ "d", cbor::Value::FromBytes(cbor::Bytes(
				data.begin() + ptrdiff_t(from),
				data.begin() + ptrdiff_t(to))) },
		}));
	}
	Reply(fd, proto::Map({
		{ "t", proto::Text("compile_done") },
		{ "total", proto::Int(int64_t(data.size())) },
	}));
	std::_Exit(0);
}

void ReleaseFreedMemory() {
#ifdef __GLIBC__
	malloc_trim(0);
#endif // __GLIBC__
}

void HandleLoad(
		int fd,
		cbor::Value message,
		Runtime &runtime,
		SocketBridge &bridge,
		std::unique_ptr<Plugin> &plugin) {
	const auto failure = [&](int64_t code, const std::string &detail) {
		Reply(fd, proto::Map({
			{ "t", proto::Text("load_failed") },
			{ "code", proto::Int(code) },
			{ "detail", proto::Text(detail) },
		}));
	};
	const auto compiledField = message.find("compiled");
	const auto compiledBytes = compiledField ? compiledField->asBytes() : nullptr;
	const auto wasm = message.find("wasm");
	const auto bytes = compiledBytes
		? compiledBytes
		: (wasm ? wasm->asBytes() : nullptr);
	if (!bytes) {
		return failure(-1, "the load message has no module");
	}
	auto process = ProcessSpec();
	const auto strings = [&](const char *key, std::vector<std::string> &out) {
		const auto found = message.find(key);
		const auto list = found ? found->asArray() : nullptr;
		for (const auto &item : list ? *list : cbor::Array()) {
			if (const auto text = item.asText()) {
				out.push_back(*text);
			}
		}
	};
	strings("args", process.args);
	strings("env", process.env);
	auto builder = VfsBuilder();
	if (const auto files = message.find("files")) {
		const auto list = files->asArray();
		for (const auto &entry : list ? *list : cbor::Array()) {
			const auto path = proto::TextField(entry, "p");
			const auto data = entry.find("d");
			const auto content = data ? data->asBytes() : nullptr;
			if (!content || builder.addFile(path, *content) != Errc::Ok) {
				return failure(-2, "bad file entry: " + path);
			}
		}
	}
	auto loaded = compiledBytes
		? runtime.loadCompiled(
			*bytes,
			bridge,
			LimitsFrom(message),
			builder.finish(),
			process)
		: runtime.load(
			*bytes,
			bridge,
			LimitsFrom(message),
			builder.finish(),
			process);
	if (!loaded.ok()) {
		return failure(
			int64_t(loaded.error().error),
			loaded.error().detail);
	}
	plugin = std::move(loaded.value());
	Reply(fd, proto::Map({ { "t", proto::Text("loaded") } }));
}

class HostDispatcher final : public Dispatcher {
public:
	explicit HostDispatcher(int fd) : _fd(fd) {
	}

	Dispatch next() override {
		while (true) {
			auto frame = ipc::ReadFrame(_fd, ipc::TrustedLimits());
			if (!frame.ok()) {
				return {};
			}
			const auto type = proto::TypeOf(frame.value());
			if (type == "stop") {
				return {};
			} else if (type == "init" || type == "event") {
				const auto value = frame.value().find("v");
				_open = proto::IntField(frame.value(), "id");
				return Dispatch{
					.kind = (type == "init")
						? Dispatch::Kind::Init
						: Dispatch::Kind::Event,
					.id = _open,
					.value = value ? *value : cbor::Value(),
				};
			}
			Reply(_fd, proto::Map({
				{ "t", proto::Text("protocol_error") },
				{ "detail", proto::Text(type) },
			}));
		}
	}

	void finished(int64_t id, int32_t status) override {
		Reply(_fd, proto::Map({
			{ "t", proto::Text("done") },
			{ "id", proto::Int(id) },
			{ "status", proto::Int(status) },
		}));
		if (_open == id) {
			_open = -1;
		}
	}

	[[nodiscard]] int64_t open() const {
		return _open;
	}

private:
	int _fd = -1;
	int64_t _open = -1;

};

void RunCommand(int fd, Plugin &plugin) {
	auto dispatcher = HostDispatcher(fd);
	const auto result = plugin.runCommand(dispatcher);
	if (dispatcher.open() >= 0) {
		Reply(fd, proto::Map({
			{ "t", proto::Text("failed") },
			{ "id", proto::Int(dispatcher.open()) },
			{ "error", proto::Int(result.ok()
				? int64_t(CallError::Dead)
				: int64_t(result.error())) },
			{ "detail", proto::Text(plugin.lastError()) },
		}));
	}
}

void HandleCall(
		int fd,
		const cbor::Value &message,
		Plugin &plugin,
		bool init) {
	const auto id = proto::IntField(message, "id");
	const auto value = message.find("v");
	const auto input = value ? *value : cbor::Value();
	const auto result = init ? plugin.init(input) : plugin.deliver(input);
	if (result.ok()) {
		Reply(fd, proto::Map({
			{ "t", proto::Text("done") },
			{ "id", proto::Int(id) },
			{ "status", proto::Int(result.value()) },
		}));
	} else {
		Reply(fd, proto::Map({
			{ "t", proto::Text("failed") },
			{ "id", proto::Int(id) },
			{ "error", proto::Int(int64_t(result.error())) },
			{ "detail", proto::Text(plugin.lastError()) },
		}));
	}
}

} // namespace

int main(int argc, char **argv) {
	auto fd = -1;
	auto layers = sandbox::kLayersAll;
	auto selftest = std::string();
	auto compileOnly = false;
	auto addressSpaceMb = uint64_t(0);
	auto cpuSeconds = uint64_t(0);
	auto runtimeOptions = RuntimeOptions();
	for (auto i = 1; i < argc; ++i) {
		const auto argument = std::string(argv[i]);
		if (argument.rfind("--ipc-fd=", 0) == 0) {
			fd = std::atoi(argument.c_str() + 9);
#ifdef FGPL_UNSANDBOXED_TEST_HOST
		} else if (argument == "--layers=none") {
			layers = 0;
#endif
		} else if (argument.rfind("--layers=", 0) == 0) {
			return 64;
		} else if (argument.rfind("--address-space-mb=", 0) == 0) {
			addressSpaceMb = std::strtoull(argument.c_str() + 19, nullptr, 10);
		} else if (argument.rfind("--reservation-mb=", 0) == 0) {
			runtimeOptions.memoryReservation = std::strtoull(
				argument.c_str() + 17,
				nullptr,
				10) << 20;
			runtimeOptions.memoryGuard = 64 << 10;
		} else if (argument.rfind("--cpu-seconds=", 0) == 0) {
			cpuSeconds = std::strtoull(argument.c_str() + 14, nullptr, 10);
		} else if (argument == "--compile-only") {
			compileOnly = true;
		} else if (argument == "--winch") {
			runtimeOptions.baselineCompiler = true;
		} else if (argument.rfind("--selftest=", 0) == 0) {
			selftest = argument.substr(11);
		}
	}
	if (fd < 3) {
		return 64;
	}
	auto options = sandbox::Options();
	options.ipcFd = fd;
	options.layers = layers;
	options.addressSpaceBytes = addressSpaceMb << 20;
	options.cpuSeconds = cpuSeconds;
	auto restricted = sandbox::Restrict(options);
	if (!restricted.ok()) {
		Fatal(fd, restricted.error());
	}
	auto runtime = Runtime(runtimeOptions);
	auto locked = sandbox::Lock(options, restricted.value());
	if (!locked.ok()) {
		Fatal(fd, locked.error());
	}
	const auto report = locked.value();
	if (layers == sandbox::kLayersAll
		&& !(report.rlimits && report.landlock && report.seccomp)) {
		Fatal(fd, "the sandbox is incomplete");
	}
	Reply(fd, proto::Map({
		{ "t", proto::Text("hello") },
		{ "landlock_abi", proto::Int(report.landlockAbi) },
		{ "landlock", proto::Bool(report.landlock) },
		{ "seccomp", proto::Bool(report.seccomp) },
		{ "rlimits", proto::Bool(report.rlimits) },
		{ "engine", proto::Text(Runtime::EngineVersion()) },
	}));

	if (selftest == "open") {
		if (open("/etc/passwd", O_RDONLY) >= 0) {
			Reply(fd, proto::Map({ { "t", proto::Text("selftest_survived") } }));
		}
	} else if (selftest == "spin") {
		volatile auto counter = uint64_t(0);
		while (true) {
			counter = counter + 1;
		}
	} else if (selftest == "env") {
		auto count = int64_t(0);
		for (auto entry = environ; entry && *entry; ++entry) {
			++count;
		}
		Reply(fd, proto::Map({
			{ "t", proto::Text("selftest_env") },
			{ "count", proto::Int(count) },
		}));
	} else if (selftest == "hang") {
		while (true) {
			const auto pause = timespec{ 1, 0 };
			nanosleep(&pause, nullptr);
		}
	}

	if (compileOnly) {
		RunCompiler(fd, runtime);
	}
	auto bridge = SocketBridge(fd);
	auto plugin = std::unique_ptr<Plugin>();
	while (true) {
		auto frame = ipc::ReadFrame(fd, ipc::TrustedLimits());
		if (!frame.ok()) {
			return (frame.error() == ipc::IoError::Closed) ? 0 : 3;
		}
		const auto type = proto::TypeOf(frame.value());
		if (type == "stop") {
			return 0;
		} else if (type == "load") {
			HandleLoad(fd, std::move(frame.value()), runtime, bridge, plugin);
			// The load frame is gone by now, give its heap back.
			ReleaseFreedMemory();
			if (plugin && plugin->isCommand()) {
				RunCommand(fd, *plugin);
			}
		} else if ((type == "init" || type == "event") && plugin) {
			HandleCall(fd, frame.value(), *plugin, type == "init");
		} else {
			Reply(fd, proto::Map({
				{ "t", proto::Text("protocol_error") },
				{ "detail", proto::Text(type) },
			}));
		}
	}
}
