#include "host_client.h"

#include "ipc.h"
#include "protocol.h"

#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
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
		return client->died("the host did not start");
	}
	const auto type = proto::TypeOf(hello.value());
	if (type == "fatal") {
		return Failure(
			ClientError::Sandbox,
			proto::TextField(hello.value(), "detail"));
	} else if (type != "hello") {
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
	return client;
}

Result<std::monostate, ClientFailure> HostClient::load(
		const LoadRequest &request) {
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
		{ "wasm", cbor::Value::FromBytes(request.wasm) },
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
		if (reply.error() == ipc::IoError::Timeout) {
			kill();
			return Failure(ClientError::Timeout, "loading the module");
		}
		return died("loading the module");
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
	return Failure(ClientError::Protocol, "unexpected reply to load: " + type);
}

Result<int32_t, ClientFailure> HostClient::run(
		const char *type,
		const cbor::Value &value,
		Bridge &bridge) {
	const auto id = _nextId++;
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
			if (frame.error() == ipc::IoError::Timeout) {
				kill();
				return Failure(ClientError::Timeout, "the host did not answer");
			} else if (frame.error() == ipc::IoError::Malformed
				|| frame.error() == ipc::IoError::TooLarge) {
				kill();
				return Failure(ClientError::Protocol, "a malformed message");
			}
			return died("waiting for the host");
		}
		const auto kind = proto::TypeOf(frame.value());
		if (kind == "log") {
			bridge.log(proto::TextField(frame.value(), "line"));
		} else if (kind == "call") {
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
	auto frame = ipc::ReadFrame(
		_fd,
		ipc::StrictLimits(),
		ipc::kMaxFrameBytes,
		timeoutMs);
	if (!frame.ok()) {
		if (frame.error() == ipc::IoError::Timeout) {
			return Failure(ClientError::Timeout, "no message");
		}
		return died("waiting for a message");
	}
	return std::move(frame.value());
}

} // namespace fgpl
