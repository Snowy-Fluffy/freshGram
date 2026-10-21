#pragma once

#include "cbor.h"
#include "ipc.h"
#include "result.h"
#include "runtime.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fgpl {

enum class ClientError : uint8_t {
	SpawnFailed,
	HostDied,
	Timeout,
	Protocol,
	Sandbox,
	LoadFailed,
	CallFailed,
};

struct ClientFailure {
	ClientError error = ClientError::Protocol;
	std::string detail;
	int signal = 0;
	int exitCode = 0;
	int code = 0;
};

struct HostConfig {
	std::string path;
	uint32_t layers = 7;
	bool requireLandlock = false;
	std::string selftest;
	std::vector<std::string> extraArguments;
	int addressSpaceMb = 1024;
	int reservationMb = 16;
	size_t maxLogBytes = size_t(1) << 20;
	int maxCalls = 512;
	int cpuSeconds = 0;
	int sendTimeoutMs = 5000;
	int startupTimeoutMs = 10000;
};

struct SandboxInfo {
	int landlockAbi = 0;
	bool landlock = false;
	bool seccomp = false;
	bool rlimits = false;
	std::string engine;
};

struct LoadRequest {
	cbor::Bytes wasm;
	cbor::Bytes compiled;
	std::vector<std::pair<std::string, cbor::Bytes>> files;
	RuntimeLimits limits;
	std::vector<std::string> args;
	std::vector<std::string> env;
};

class HostClient {
public:
	~HostClient();

	[[nodiscard]] static Result<std::unique_ptr<HostClient>, ClientFailure>
	Start(const HostConfig &config);

	[[nodiscard]] static Result<cbor::Bytes, ClientFailure> Compile(
		const HostConfig &config,
		const cbor::Bytes &wasm,
		size_t maxBytes = size_t(128) << 20,
		int timeoutMs = 120000);

	[[nodiscard]] const SandboxInfo &sandbox() const {
		return _sandbox;
	}

	[[nodiscard]] Result<std::monostate, ClientFailure> load(
		const LoadRequest &request);
	[[nodiscard]] Result<int32_t, ClientFailure> init(
		const cbor::Value &info,
		Bridge &bridge);
	[[nodiscard]] Result<int32_t, ClientFailure> deliver(
		const cbor::Value &event,
		Bridge &bridge);

	[[nodiscard]] Result<cbor::Value, ClientFailure> waitFor(int timeoutMs);

	void kill();

	[[nodiscard]] int pid() const {
		return _pid;
	}

private:
	HostClient() = default;

	[[nodiscard]] Result<int32_t, ClientFailure> run(
		const char *type,
		const cbor::Value &value,
		Bridge &bridge);
	[[nodiscard]] ClientFailure died(const std::string &context);
	[[nodiscard]] ClientFailure fail(
		ipc::IoError error,
		const std::string &context);

	int _fd = -1;
	int _pid = -1;
	int _timeoutMs = 500;
	int64_t _nextId = 1;
	size_t _maxLogBytes = 0;
	int _maxCalls = 0;
	SandboxInfo _sandbox;

};

} // namespace fgpl
