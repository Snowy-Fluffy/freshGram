#include "fgpl/host_client.h"

#include "fgpl/ipc.h"
#include "fgpl/package.h"
#include "fgpl/protocol.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace fgpl {
namespace {

constexpr auto kCallGraceMs = 3000;

[[nodiscard]] ClientFailure Failure(ClientError error, std::string detail) {
	return ClientFailure{ .error = error, .detail = std::move(detail) };
}

} // namespace

HostClient::~HostClient() {
	kill();
}

int HostClient::reap(bool block, os::ExitInfo &info) {
	const auto lock = std::scoped_lock(_reap);
	if (_pid.load() <= 0 || !_child) {
		return -1;
	}
	if (block) {
		_child->wait(info);
	} else if (!_child->tryWait(info)) {
		return 0;
	}
	_pid.store(-1);
	return 1;
}

void HostClient::abort() {
	const auto lock = std::scoped_lock(_reap);
	if (_pid.load() > 0 && _child) {
		_child->terminate();
	}
}

void HostClient::kill() {
	if (_fd >= 0) {
		os::CloseChannel(_fd);
		_fd = os::kNoHandle;
	}
	if (_pid.load() > 0) {
		abort();
		auto info = os::ExitInfo();
		reap(true, info);
	}
}

ClientFailure HostClient::died(const std::string &context) {
	auto failure = Failure(ClientError::HostDied, context);
	if (_pid.load() > 0) {
		auto info = os::ExitInfo();
		auto waited = reap(false, info);
		for (auto i = 0; waited == 0 && i != 200; ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			waited = reap(false, info);
		}
		if (waited == 0) {
			abort();
			waited = reap(true, info);
			failure.detail += ": the host stopped answering";
		}
		if (waited == 1) {
			if (info.signaled) {
				failure.signal = info.signal;
				if (info.signal != 0
					&& info.signal == os::SandboxViolationSignal()) {
					failure.detail += ": blocked by the seccomp filter";
				}
			} else {
				failure.exitCode = info.code;
			}
		}
	}
	return failure;
}

ClientFailure HostClient::fail(ipc::IoError error, const std::string &context) {
#ifdef _WIN32
	if (const auto code = os::ChannelError()) {
		return fail(error, context, " (system error " + std::to_string(code) + ")");
	}
#endif // _WIN32
	return fail(error, context, std::string());
}

ClientFailure HostClient::fail(
		ipc::IoError error,
		const std::string &contextBase,
		const std::string &note) {
	const auto context = contextBase + note;
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
	auto spec = os::SpawnSpec();
	spec.path = config.path;
	spec.executable = config.executableFd;
	spec.addressSpaceMb = config.addressSpaceMb;
	spec.cpuSeconds = config.cpuSeconds;
	spec.sendTimeoutMs = config.sendTimeoutMs;
	spec.sandboxed = !config.unsandboxedTestHost;
	if (config.unsandboxedTestHost) {
		spec.arguments.push_back("--layers=none");
	}
	if (!config.selftest.empty()) {
		spec.arguments.push_back("--selftest=" + config.selftest);
	}
	if (config.addressSpaceMb > 0) {
		spec.arguments.push_back(
			"--address-space-mb=" + std::to_string(config.addressSpaceMb));
	}
	if (config.cpuSeconds > 0) {
		spec.arguments.push_back(
			"--cpu-seconds=" + std::to_string(config.cpuSeconds));
	}
	if (config.reservationMb > 0) {
		spec.arguments.push_back(
			"--reservation-mb=" + std::to_string(config.reservationMb));
	}
	for (const auto &argument : config.extraArguments) {
		spec.arguments.push_back(argument);
	}
	auto channel = os::kNoHandle;
	auto child = os::Child::Spawn(spec, channel);
	if (!child.ok()) {
		return Failure(ClientError::SpawnFailed, child.error());
	}
	auto client = std::unique_ptr<HostClient>(new HostClient());
	client->_fd = channel;
	client->_pid.store(child.value()->id());
	client->_child = std::move(child.value());

	auto hello = ipc::ReadFrame(
		client->_fd,
		ipc::StrictLimits(),
		ipc::kMaxFrameBytes,
		config.startupTimeoutMs);
	if (!hello.ok()) {
		return client->fail(hello.error(), "the host did not start");
	}
#ifdef __APPLE__
	// RLIMIT_AS does nothing on macOS, so the cap is checked from this side.
	client->_rssLimit = uint64_t(std::max(config.addressSpaceMb, 0)) << 20;
#endif // __APPLE__
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
	client->_sandbox.filesystem = flag("fs");
	client->_sandbox.processes = flag("proc");
	client->_sandbox.limits = flag("limits");
	client->_sandbox.platform = proto::TextField(hello.value(), "platform");
	client->_sandbox.engine = proto::TextField(hello.value(), "engine");
	if (!config.unsandboxedTestHost
		&& !(client->_sandbox.filesystem
			&& client->_sandbox.processes
			&& client->_sandbox.limits)) {
		client->kill();
		return Failure(
			ClientError::Sandbox,
			"the host started without its full sandbox");
	}
	return client;
}

Result<cbor::Bytes, ClientFailure> HostClient::Compile(
		const HostConfig &config,
		const cbor::Bytes &wasm,
		size_t maxBytes,
		int timeoutMs,
		const std::atomic<bool> *cancel) {
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
		if (cancel && cancel->load()) {
			client.kill();
			return Failure(ClientError::Timeout, "cancelled");
		}
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - std::chrono::steady_clock::now()).count();
		auto frame = ipc::ReadFrame(
			client._fd,
			ipc::StrictLimits(),
			ipc::kMaxFrameBytes,
			int(std::clamp<int64_t>(left, 0, 200)));
		if (!frame.ok()) {
			if (frame.error() == ipc::IoError::Timeout && left > 200) {
				continue;
			}
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
	_hostCallMs = int(request.limits.maxHostCallMs);
	_startMs = int(request.limits.startTimeoutMs);
	_fresh = true;
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
			{ "start", proto::Int(int64_t(request.limits.startTimeoutMs)) },
			{ "message", proto::Int(int64_t(request.limits.maxMessageBytes)) },
			{ "hostcall", proto::Int(int64_t(request.limits.maxHostCallMs)) },
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
	const auto grace = _fresh ? _startMs : 0;
	_fresh = false;
	auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(_timeoutMs + kCallGraceMs + grace);
	auto hosted = std::chrono::milliseconds(0);
	while (true) {
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - std::chrono::steady_clock::now()).count();
		if (_rssLimit != 0 && os::ProcessUsage(_pid.load()).rssBytes > _rssLimit) {
			kill();
			return Failure(ClientError::HostDied, "the host used too much memory");
		}
		// Slices keep the memory check frequent on a host that sends nothing.
		const auto slice = (_rssLimit != 0) ? std::min<int64_t>(left, 100) : left;
		auto frame = ipc::ReadFrame(
			_fd,
			ipc::StrictLimits(),
			ipc::kMaxFrameBytes,
			int(slice > 0 ? slice : 0));
		if (!frame.ok()) {
			if (frame.error() == ipc::IoError::Timeout && slice < left) {
				continue;
			}
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
			const auto started = std::chrono::steady_clock::now();
			const auto response = bridge.call(request ? *request : cbor::Value());
			const auto spent = std::min(
				std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - started),
				std::chrono::milliseconds(_hostCallMs) - hosted);
			if (spent.count() > 0) {
				hosted += spent;
				deadline += spent;
			}
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
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(std::max(timeoutMs, 0));
	while (true) {
		if (_rssLimit != 0 && os::ProcessUsage(_pid.load()).rssBytes > _rssLimit) {
			kill();
			return Failure(ClientError::HostDied, "the host used too much memory");
		}
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - std::chrono::steady_clock::now()).count();
		const auto slice = (_rssLimit != 0) ? std::min<int64_t>(left, 100) : left;
		auto frame = ipc::ReadFrame(
			_fd,
			ipc::StrictLimits(),
			ipc::kMaxFrameBytes,
			(_rssLimit != 0) ? int(std::max<int64_t>(slice, 0)) : timeoutMs);
		if (frame.ok()) {
			return std::move(frame.value());
		} else if (frame.error() == ipc::IoError::Timeout) {
			if (_rssLimit != 0 && left > slice) {
				continue;
			}
			return Failure(ClientError::Timeout, "no message");
		}
		return fail(frame.error(), "waiting for a message");
	}
}

} // namespace fgpl
