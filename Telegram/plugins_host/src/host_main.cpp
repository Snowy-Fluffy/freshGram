#include "ipc.h"
#include "protocol.h"
#include "runtime.h"
#include "sandbox_linux.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

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

void HandleLoad(
		int fd,
		const cbor::Value &message,
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
	const auto wasm = message.find("wasm");
	const auto bytes = wasm ? wasm->asBytes() : nullptr;
	if (!bytes) {
		return failure(-1, "the load message has no module");
	}
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
	auto loaded = runtime.load(
		*bytes,
		bridge,
		LimitsFrom(message),
		builder.finish());
	if (!loaded.ok()) {
		return failure(
			int64_t(loaded.error().error),
			loaded.error().detail);
	}
	plugin = std::move(loaded.value());
	Reply(fd, proto::Map({ { "t", proto::Text("loaded") } }));
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
	auto requireLandlock = false;
	auto selftest = std::string();
	auto addressSpaceMb = uint64_t(0);
	auto runtimeOptions = RuntimeOptions();
	for (auto i = 1; i < argc; ++i) {
		const auto argument = std::string(argv[i]);
		if (argument.rfind("--ipc-fd=", 0) == 0) {
			fd = std::atoi(argument.c_str() + 9);
		} else if (argument == "--layers=none") {
			layers = 0;
		} else if (argument == "--layers=landlock") {
			layers = sandbox::kLayerRlimits | sandbox::kLayerLandlock;
		} else if (argument == "--require-landlock") {
			requireLandlock = true;
		} else if (argument.rfind("--address-space-mb=", 0) == 0) {
			addressSpaceMb = std::strtoull(argument.c_str() + 19, nullptr, 10);
		} else if (argument.rfind("--reservation-mb=", 0) == 0) {
			runtimeOptions.memoryReservation = std::strtoull(
				argument.c_str() + 17,
				nullptr,
				10) << 20;
			runtimeOptions.memoryGuard = 64 << 10;
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
	options.requireLandlock = requireLandlock;
	options.addressSpaceBytes = addressSpaceMb << 20;
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
	Reply(fd, proto::Map({
		{ "t", proto::Text("hello") },
		{ "landlock_abi", proto::Int(report.landlockAbi) },
		{ "landlock", proto::Bool(report.landlock) },
		{ "seccomp", proto::Bool(report.seccomp) },
		{ "rlimits", proto::Bool(report.rlimits) },
	}));

	if (selftest == "open") {
		if (open("/etc/passwd", O_RDONLY) >= 0) {
			Reply(fd, proto::Map({ { "t", proto::Text("selftest_survived") } }));
		}
	} else if (selftest == "hang") {
		while (true) {
			const auto pause = timespec{ 1, 0 };
			nanosleep(&pause, nullptr);
		}
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
			HandleLoad(fd, frame.value(), runtime, bridge, plugin);
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
