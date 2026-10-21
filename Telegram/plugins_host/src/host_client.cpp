#include "host_client.h"

#include "ipc.h"
#include "protocol.h"

#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

extern char **environ;

namespace fgpl {
namespace {

constexpr auto kHostFd = 3;
constexpr auto kCallGraceMs = 3000;

[[nodiscard]] ClientFailure Failure(ClientError error, std::string detail) {
	return ClientFailure{ .error = error, .detail = std::move(detail) };
}

} // namespace

HostClient::~HostClient() {
	kill();
}

void HostClient::kill() {
	if (_fd >= 0) {
		close(_fd);
		_fd = -1;
	}
	if (_pid > 0) {
		::kill(_pid, SIGKILL);
		auto status = 0;
		waitpid(_pid, &status, 0);
		_pid = -1;
	}
}

ClientFailure HostClient::died(const std::string &context) {
	auto failure = Failure(ClientError::HostDied, context);
	if (_pid > 0) {
		auto status = 0;
		auto waited = waitpid(_pid, &status, WNOHANG);
		for (auto i = 0; waited == 0 && i != 200; ++i) {
			usleep(5000);
			waited = waitpid(_pid, &status, WNOHANG);
		}
		if (waited == 0) {
			::kill(_pid, SIGKILL);
			waited = waitpid(_pid, &status, 0);
			failure.detail += ": the host stopped answering";
		}
		if (waited == _pid) {
			_pid = -1;
			if (WIFSIGNALED(status)) {
				failure.signal = WTERMSIG(status);
				if (failure.signal == SIGSYS) {
					failure.detail += ": blocked by the seccomp filter";
				}
			} else if (WIFEXITED(status)) {
				failure.exitCode = WEXITSTATUS(status);
			}
		}
	}
	return failure;
}

ClientFailure HostClient::fail(ipc::IoError error, const std::string &context) {
	switch (error) {
	case ipc::IoError::Closed:
	case ipc::IoError::Failed:
		return died(context);
	case ipc::IoError::Timeout:
		kill();
		return Failure(ClientError::Timeout, context);
	case ipc::IoError::TooLarge:
	case ipc::IoError::Malformed:
		kill();
		return Failure(
			ClientError::Protocol,
			context + ": a malformed or oversized message");
	}
	return Failure(ClientError::Protocol, context);
}

Result<std::unique_ptr<HostClient>, ClientFailure> HostClient::Start(
		const HostConfig &config) {
	int pair[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
		return Failure(ClientError::SpawnFailed, std::strerror(errno));
	}
	signal(SIGPIPE, SIG_IGN);
	auto actions = posix_spawn_file_actions_t();
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, pair[1], kHostFd);
	auto arguments = std::vector<std::string>{
		config.path,
		"--ipc-fd=" + std::to_string(kHostFd),
	};
	if (config.layers == 0) {
		arguments.push_back("--layers=none");
	} else if (config.layers == 3) {
		arguments.push_back("--layers=landlock");
	}
	if (config.requireLandlock) {
		arguments.push_back("--require-landlock");
	}
	if (!config.selftest.empty()) {
		arguments.push_back("--selftest=" + config.selftest);
	}
	if (config.addressSpaceMb > 0) {
		arguments.push_back(
			"--address-space-mb=" + std::to_string(config.addressSpaceMb));
	}
	if (config.cpuSeconds > 0) {
		arguments.push_back("--cpu-seconds=" + std::to_string(config.cpuSeconds));
	}
	if (config.reservationMb > 0) {
		arguments.push_back(
			"--reservation-mb=" + std::to_string(config.reservationMb));
	}
	for (const auto &argument : config.extraArguments) {
		arguments.push_back(argument);
	}
	auto argv = std::vector<char *>();
	for (auto &argument : arguments) {
		argv.push_back(argument.data());
	}
	argv.push_back(nullptr);
	auto pid = pid_t();
	const auto spawned = posix_spawn(
		&pid,
		config.path.c_str(),
		&actions,
		nullptr,
		argv.data(),
		environ);
	posix_spawn_file_actions_destroy(&actions);
	close(pair[1]);
	if (config.sendTimeoutMs > 0) {
		const auto timeout = timeval{
			.tv_sec = config.sendTimeoutMs / 1000,
			.tv_usec = (config.sendTimeoutMs % 1000) * 1000,
		};
		setsockopt(pair[0], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	}
	if (spawned != 0) {
		close(pair[0]);
		return Failure(ClientError::SpawnFailed, std::strerror(spawned));
	}
	auto client = std::unique_ptr<HostClient>(new HostClient());
	client->_fd = pair[0];
	client->_pid = pid;

	auto hello = ipc::ReadFrame(
		client->_fd,
		ipc::StrictLimits(),
		ipc::kMaxFrameBytes,
		config.startupTimeoutMs);
	if (!hello.ok()) {
		return client->fail(hello.error(), "the host did not start");
	}
	client->_maxLogBytes = config.maxLogBytes;
	client->_maxCalls = config.maxCalls;
	const auto type = proto::TypeOf(hello.value());
	if (type == "fatal") {
		client->kill();
		return Failure(
			ClientError::Sandbox,
			proto::TextField(hello.value(), "detail"));
	} else if (type != "hello") {
		client->kill();
		return Failure(ClientError::Protocol, "unexpected first message");
	}
	const auto flag = [&](const char *key) {
		const auto found = hello.value().find(key);
		return found && found->asBool().value_or(false);
	};
	client->_sandbox.landlockAbi = int(proto::IntField(
		hello.value(),
		"landlock_abi"));
	client->_sandbox.landlock = flag("landlock");
	client->_sandbox.seccomp = flag("seccomp");
	client->_sandbox.rlimits = flag("rlimits");
	client->_sandbox.engine = proto::TextField(hello.value(), "engine");
	return client;
}

Result<cbor::Bytes, ClientFailure> HostClient::Compile(
		const HostConfig &config,
		const cbor::Bytes &wasm,
		size_t maxBytes,
		int timeoutMs) {
	auto compileConfig = config;
	compileConfig.extraArguments.push_back("--compile-only");
	auto started = Start(compileConfig);
	if (!started.ok()) {
		return started.error();
	}
	auto &client = *started.value();
	if (!ipc::WriteFrame(client._fd, proto::Map({
			{ "t", proto::Text("compile") },
			{ "wasm", cbor::Value::FromBytes(wasm) },
		})).ok()) {
		return client.died("sending the module");
	}
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timeoutMs);
	auto result = cbor::Bytes();
	auto expected = int64_t(0);
	while (true) {
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - std::chrono::steady_clock::now()).count();
		auto frame = ipc::ReadFrame(
			client._fd,
			ipc::StrictLimits(),
			ipc::kMaxFrameBytes,
			int(left > 0 ? left : 0));
		if (!frame.ok()) {
			return client.fail(frame.error(), "compiling the module");
		}
		const auto type = proto::TypeOf(frame.value());
		if (type == "compiled") {
			const auto data = frame.value().find("d");
			const auto bytes = data ? data->asBytes() : nullptr;
			if (!bytes
				|| proto::IntField(frame.value(), "i") != expected
				|| result.size() + bytes->size() > maxBytes) {
				client.kill();
				return Failure(ClientError::Protocol, "a bad compiled chunk");
			}
			++expected;
			result.insert(result.end(), bytes->begin(), bytes->end());
		} else if (type == "compile_done") {
			if (proto::IntField(frame.value(), "total") != int64_t(result.size())) {
				client.kill();
				return Failure(ClientError::Protocol, "a wrong compiled size");
			}
			return result;
		} else if (type == "compile_failed") {
			auto failure = Failure(
				ClientError::LoadFailed,
				proto::TextField(frame.value(), "detail"));
			client.kill();
			return failure;
		} else {
			client.kill();
			return Failure(ClientError::Protocol, "unexpected message: " + type);
		}
	}
}

Result<std::monostate, ClientFailure> HostClient::load(
		const LoadRequest &request) {
	if (_fd < 0) {
		return Failure(ClientError::HostDied, "the host is not running");
	}
	auto files = cbor::Array();
	for (const auto &file : request.files) {
		files.push_back(proto::Map({
			{ "p", proto::Text(file.first) },
			{ "d", cbor::Value::FromBytes(file.second) },
		}));
	}
	const auto texts = [](const std::vector<std::string> &list) {
		auto result = cbor::Array();
		for (const auto &item : list) {
			result.push_back(proto::Text(item));
		}
		return cbor::Value::FromArray(std::move(result));
	};
	_timeoutMs = int(request.limits.callTimeoutMs);
	const auto sent = ipc::WriteFrame(_fd, proto::Map({
		{ "t", proto::Text("load") },
		{ request.compiled.empty() ? "wasm" : "compiled",
			cbor::Value::FromBytes(request.compiled.empty()
				? request.wasm
				: request.compiled) },
		{ "files", cbor::Value::FromArray(std::move(files)) },
		{ "args", texts(request.args) },
		{ "env", texts(request.env) },
		{ "limits", proto::Map({
			{ "memory", proto::Int(int64_t(request.limits.memoryBytes)) },
			{ "timeout", proto::Int(int64_t(request.limits.callTimeoutMs)) },
			{ "message", proto::Int(int64_t(request.limits.maxMessageBytes)) },
		}) },
	}));
	if (!sent.ok()) {
		return died("sending the module");
	}
	auto reply = ipc::ReadFrame(
		_fd,
		ipc::StrictLimits(),
		ipc::kMaxFrameBytes,
		_timeoutMs + kCallGraceMs + 10000);
	if (!reply.ok()) {
		return fail(reply.error(), "loading the module");
	}
	const auto type = proto::TypeOf(reply.value());
	if (type == "loaded") {
		return std::monostate();
	} else if (type == "load_failed") {
		auto failure = Failure(
			ClientError::LoadFailed,
			proto::TextField(reply.value(), "detail"));
		failure.code = int(proto::IntField(reply.value(), "code"));
		return failure;
	}
	kill();
	return Failure(ClientError::Protocol, "unexpected reply to load: " + type);
}

Result<int32_t, ClientFailure> HostClient::run(
		const char *type,
		const cbor::Value &value,
		Bridge &bridge) {
	if (_fd < 0) {
		return Failure(ClientError::HostDied, "the host is not running");
	}
	const auto id = _nextId++;
	auto logBytes = size_t(0);
	auto calls = 0;
	if (!ipc::WriteFrame(_fd, proto::Map({
			{ "t", proto::Text(type) },
			{ "id", proto::Int(id) },
			{ "v", value },
		})).ok()) {
		return died("sending a message");
	}
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(_timeoutMs + kCallGraceMs);
	while (true) {
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - std::chrono::steady_clock::now()).count();
		auto frame = ipc::ReadFrame(
			_fd,
			ipc::StrictLimits(),
			ipc::kMaxFrameBytes,
			int(left > 0 ? left : 0));
		if (!frame.ok()) {
			return fail(frame.error(), "waiting for the host");
		}
		const auto kind = proto::TypeOf(frame.value());
		if (kind == "log") {
			const auto line = proto::TextField(frame.value(), "line");
			logBytes += line.size() + 1;
			if (logBytes <= _maxLogBytes) {
				bridge.log(line);
			}
		} else if (kind == "call") {
			if (++calls > _maxCalls) {
				kill();
				return Failure(ClientError::Protocol, "too many calls");
			}
			const auto request = frame.value().find("v");
			const auto response = bridge.call(request ? *request : cbor::Value());
			if (!ipc::WriteFrame(_fd, proto::Map({
					{ "t", proto::Text("reply") },
					{ "v", response },
				})).ok()) {
				return died("answering a call");
			}
		} else if (kind == "done" && proto::IntField(frame.value(), "id") == id) {
			return int32_t(proto::IntField(frame.value(), "status"));
		} else if (kind == "failed" && proto::IntField(frame.value(), "id") == id) {
			auto failure = Failure(
				ClientError::CallFailed,
				proto::TextField(frame.value(), "detail"));
			failure.code = int(proto::IntField(frame.value(), "error"));
			return failure;
		} else {
			kill();
			return Failure(ClientError::Protocol, "unexpected message: " + kind);
		}
	}
}

Result<int32_t, ClientFailure> HostClient::init(
		const cbor::Value &info,
		Bridge &bridge) {
	return run("init", info, bridge);
}

Result<int32_t, ClientFailure> HostClient::deliver(
		const cbor::Value &event,
		Bridge &bridge) {
	return run("event", event, bridge);
}

Result<cbor::Value, ClientFailure> HostClient::waitFor(int timeoutMs) {
	if (_fd < 0) {
		return Failure(ClientError::HostDied, "the host is not running");
	}
	auto frame = ipc::ReadFrame(
		_fd,
		ipc::StrictLimits(),
		ipc::kMaxFrameBytes,
		timeoutMs);
	if (!frame.ok()) {
		if (frame.error() == ipc::IoError::Timeout) {
			return Failure(ClientError::Timeout, "no message");
		}
		return fail(frame.error(), "waiting for a message");
	}
	return std::move(frame.value());
}

} // namespace fgpl
