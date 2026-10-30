// Runs on every platform: the host in its sandbox, and the pieces around it.
#include "check.h"
#include "fgpl/host_client.h"
#include "fgpl/os.h"
#include "fgpl/package.h"
#include "fgpl/sandbox.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace fgpl;
namespace fs = std::filesystem;

[[nodiscard]] std::string TypeOf(const cbor::Value &value) {
	const auto type = value.find("t");
	return (type && type->asText()) ? *type->asText() : std::string();
}

[[nodiscard]] std::unique_ptr<HostClient> Start(
		bool sandboxed,
		const std::string &selftest = {},
		int addressSpaceMb = 1024) {
	auto config = HostConfig();
	config.path = sandboxed ? FGPL_HOST_BINARY : FGPL_UNSANDBOXED_HOST_BINARY;
	config.unsandboxedTestHost = !sandboxed;
	config.selftest = selftest;
	config.addressSpaceMb = addressSpaceMb;
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

// A plugin that got out of Wasmtime must find nothing it can use.
[[nodiscard]] bool Survives(bool sandboxed, const std::string &selftest) {
	const auto client = Start(sandboxed, selftest);
	if (!client) {
		return false;
	}
	const auto reply = client->waitFor(sandboxed ? 5000 : 15000);
	return reply.ok() && TypeOf(reply.value()) == "selftest_survived";
}

void TestSandboxIsOn() {
	CHECK(sandbox::Supported());
	const auto client = Start(true);
	if (!client) {
		return;
	}
	CHECK(client->sandbox().filesystem);
	CHECK(client->sandbox().processes);
	CHECK(client->sandbox().limits);
	CHECK(!client->sandbox().platform.empty());
	CHECK(!client->sandbox().engine.empty());
	CHECK(client->pid() > 0);
}

void TestHostCannotReachFiles() {
	// Without the sandbox the attempt works, so it proves nothing by failing alone.
	CHECK(Survives(false, "open"));
	CHECK(!Survives(true, "open"));
}

void TestHostCannotStartProcesses() {
	CHECK(Survives(false, "spawn"));
	CHECK(!Survives(true, "spawn"));
}

void TestHostMemoryIsBounded() {
	const auto client = Start(true, "alloc", 512);
	if (!client) {
		return;
	}
	const auto reply = client->waitFor(20000);
	CHECK(!(reply.ok() && TypeOf(reply.value()) == "selftest_survived"));
}

void TestHostIsKilledAndReported() {
	const auto client = Start(true, "hang");
	if (!client) {
		return;
	}
	const auto reply = client->waitFor(300);
	CHECK(!reply.ok() && reply.error().error == ClientError::Timeout);
	client->abort();
	const auto gone = client->waitFor(5000);
	CHECK(!gone.ok() && gone.error().error == ClientError::HostDied);
	CHECK(client->pid() <= 0);
}

void TestHostGetsNoEnvironment() {
	const auto client = Start(true, "env");
	if (!client) {
		return;
	}
	const auto reply = client->waitFor(5000);
	CHECK(reply.ok() && TypeOf(reply.value()) == "selftest_env");
	const auto count = reply.ok() ? reply.value().find("count") : nullptr;
#ifdef _WIN32
	// Only the variables Windows needs to start a process: SystemRoot and windir.
	CHECK(count && count->asInt() && *count->asInt() <= 2);
#else // _WIN32
	CHECK(count && count->asInt() && *count->asInt() == 0);
#endif // _WIN32
}

void TestSealedHostCannotBeSwapped() {
	const auto copy = fs::temp_directory_path()
		/ ("fgpl-sealed-host-" + std::to_string(os::ProcessId())
			+ std::string(os::kHostFileName));
	fs::copy_file(FGPL_HOST_BINARY, copy, fs::copy_options::overwrite_existing);
	auto stream = std::ifstream(copy, std::ios::binary);
	const auto bytes = cbor::Bytes(std::istreambuf_iterator<char>(stream), {});
	stream.close();
	const auto utf8 = os::PathToUtf8(copy);

	CHECK(!os::SealExecutable(utf8, HashText(cbor::Bytes{ 1, 2, 3 })).ok());
	const auto sealed = os::SealExecutable(utf8, HashText(bytes));
	CHECK(sealed.ok());
	if (sealed.ok()) {
#ifdef _WIN32
		// The held handle denies writers, the file is the one that was checked.
		auto evil = std::ofstream(copy, std::ios::binary | std::ios::trunc);
		CHECK(!evil.is_open());
#endif // _WIN32
		auto config = HostConfig();
		config.path = utf8;
		config.executableFd = sealed.value();
		const auto started = HostClient::Start(config);
		CHECK(started.ok());
		CHECK(started.ok() && started.value()->sandbox().filesystem
			&& started.value()->sandbox().processes);
		os::CloseExecutable(sealed.value());
	}
	std::error_code error;
	fs::remove(copy, error);
}

void TestFilesInANonAsciiFolder() {
	const auto root = fs::temp_directory_path()
		/ os::PathFromUtf8("fgpl-\xD0\xB4\xD0\xB0\xD0\xBD\xD0\xBD\xD1\x8B\xD0\xB5-"
			+ std::to_string(os::ProcessId()));
	auto error = std::error_code();
	fs::remove_all(root, error);
	fs::create_directories(root, error);
	CHECK(!error);

	const auto file = root / "a.bin";
	const auto first = std::vector<uint8_t>{ 1, 2, 3 };
	const auto second = std::vector<uint8_t>{ 4, 5, 6, 7 };
	CHECK(os::WriteDurable(file, first));
	CHECK(os::WriteDurable(root / "b.bin", second));
	CHECK(os::ReplaceFileAtomic(root / "b.bin", file, error));
	CHECK(!fs::exists(root / "b.bin"));
	CHECK(fs::file_size(file) == second.size());

	CHECK(os::PathToUtf8(os::PathFromUtf8(os::PathToUtf8(file)))
		== os::PathToUtf8(file));

	fs::create_directories(root / "rev-1", error);
	fs::create_directories(root / "rev-2", error);
	CHECK(os::SetCurrent(root, "rev-1"));
	CHECK(fs::equivalent(os::ResolveCurrent(root), root / "rev-1", error));
	CHECK(os::SetCurrent(root, "rev-2"));
	CHECK(fs::equivalent(os::ResolveCurrent(root), root / "rev-2", error));
	fs::remove_all(root, error);
}

void TestRandomBytes() {
	auto first = std::vector<uint8_t>(64);
	auto second = std::vector<uint8_t>(64);
	os::RandomBytes(first);
	os::RandomBytes(second);
	CHECK(first != second);
	CHECK(first != std::vector<uint8_t>(64));
}

void TestUsageOfARunningHost() {
	const auto client = Start(true, "hang");
	if (!client) {
		return;
	}
	const auto usage = os::ProcessUsage(client->pid());
	CHECK(usage.rssBytes > 0);
}

} // namespace

int main() {
	TestSandboxIsOn();
	TestHostCannotReachFiles();
	TestHostCannotStartProcesses();
	TestHostMemoryIsBounded();
	TestHostIsKilledAndReported();
	TestHostGetsNoEnvironment();
	TestSealedHostCannotBeSwapped();
	TestFilesInANonAsciiFolder();
	TestRandomBytes();
	TestUsageOfARunningHost();
	return Failures() ? 1 : 0;
}
