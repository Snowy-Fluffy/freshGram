#pragma once

#include "fgpl/cbor.h"
#include "fgpl/ipc.h"
#include "fgpl/os.h"
#include "fgpl/result.h"
#include "fgpl/runtime.h"

#include <atomic>
#include <memory>
#include <mutex>
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
	os::Handle executableFd = os::kNoHandle;
	bool unsandboxedTestHost = false;
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
	bool filesystem = false;
	bool processes = false;
	bool limits = false;
	std::string platform;

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

[[nodiscard]] inline Result<os::Handle, std::string> SealExecutable(
		const std::string &path,
		const std::string &hash) {
	return os::SealExecutable(path, hash);
}

class HostClient {
public:
	~HostClient();

	[[nodiscard]] static Result<std::unique_ptr<HostClient>, ClientFailure>
	Start(const HostConfig &config);

	[[nodiscard]] static Result<cbor::Bytes, ClientFailure> Compile(
		const HostConfig &config,
		const cbor::Bytes &wasm,
		size_t maxBytes = size_t(128) << 20,
		int timeoutMs = 120000,
		const std::atomic<bool> *cancel = nullptr);

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
		return _pid.load();
	}

	void abort();
	[[nodiscard]] bool alive() const {
		return _fd >= 0;
	}

private:
	HostClient() = default;

	[[nodiscard]] Result<int32_t, ClientFailure> run(
		const char *type,
		const cbor::Value &value,
		Bridge &bridge);
	[[nodiscard]] ClientFailure died(const std::string &context);
	// 1 when the host was reaped now, 0 while it still runs, -1 when it is gone.
	int reap(bool block, os::ExitInfo &info);
	[[nodiscard]] ClientFailure fail(
		ipc::IoError error,
		const std::string &context);
	[[nodiscard]] ClientFailure fail(
		ipc::IoError error,
		const std::string &context,
		const std::string &note);

	os::Handle _fd = os::kNoHandle;
	std::unique_ptr<os::Child> _child;
	std::atomic<int> _pid{ -1 };
	std::mutex _reap;
	int _timeoutMs = 500;
	uint64_t _rssLimit = 0;
	int _startMs = 10000;
	int _hostCallMs = 60000;
	bool _fresh = false;
	int64_t _nextId = 1;
	size_t _maxLogBytes = 0;
	int _maxCalls = 0;
	SandboxInfo _sandbox;

};

} // namespace fgpl
