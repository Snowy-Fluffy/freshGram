#pragma once

#include "fgpl/result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fgpl::os {

// A file descriptor on POSIX, a HANDLE on Windows.
using Handle = intptr_t;
constexpr auto kNoHandle = Handle(-1);

#ifdef _WIN32
inline constexpr auto kHostFileName = "fgpl_plugin_host.exe";
#else // _WIN32
inline constexpr auto kHostFileName = "fgpl_plugin_host";
#endif // _WIN32

#ifdef _WIN32
// Writes to a channel give up after this long, the Windows counterpart of SO_SNDTIMEO.
void SetSendTimeout(Handle channel, int milliseconds);
[[nodiscard]] int SendTimeout(Handle channel);
#endif // _WIN32

[[nodiscard]] int ProcessId();
void RandomBytes(std::span<uint8_t> out);

// Paths travel as UTF-8 strings, std::filesystem would read them as ANSI on Windows.
[[nodiscard]] std::filesystem::path PathFromUtf8(std::string_view text);
[[nodiscard]] std::string PathToUtf8(const std::filesystem::path &path);

// Creates or truncates the file and flushes it to the disk.
[[nodiscard]] bool WriteDurable(
	const std::filesystem::path &file,
	std::span<const uint8_t> bytes);
void SyncDirectory(const std::filesystem::path &directory);
// Replaces the target atomically, retries while another process holds it.
[[nodiscard]] bool ReplaceFile(
	const std::filesystem::path &from,
	const std::filesystem::path &to,
	std::error_code &error);
void MakeExecutable(const std::filesystem::path &file);
// Points root/current at root/name: a symlink, or a pointer file on Windows.
[[nodiscard]] bool SetCurrent(
	const std::filesystem::path &root,
	const std::string &name);
[[nodiscard]] std::filesystem::path ResolveCurrent(
	const std::filesystem::path &root);

// Checks the host against the signed hash and keeps it from changing until closed.
[[nodiscard]] Result<Handle, std::string> SealExecutable(
	const std::string &path,
	const std::string &hash);
void CloseExecutable(Handle handle);

struct Usage {
	uint64_t cpuMs = 0;
	uint64_t rssBytes = 0;
};
[[nodiscard]] Usage ProcessUsage(int pid);

struct ExitInfo {
	// A signal on POSIX, an exception code such as 0xC0000005 on Windows.
	bool signaled = false;
	int signal = 0;
	int code = 0;
};

struct SpawnSpec {
	std::string path;
	Handle executable = kNoHandle;
	// Everything after the program name, the channel argument is added by Spawn.
	std::vector<std::string> arguments;
	int addressSpaceMb = 0;
	int cpuSeconds = 0;
	int sendTimeoutMs = 0;
	bool sandboxed = true;
};

class Child {
public:
	~Child();

	// Starts the host with one end of a new channel as its only handle.
	[[nodiscard]] static Result<std::unique_ptr<Child>, std::string> Spawn(
		const SpawnSpec &spec,
		Handle &channel);

	[[nodiscard]] int id() const;
	void terminate();
	[[nodiscard]] bool tryWait(ExitInfo &info);
	void wait(ExitInfo &info);

private:
	struct Native;
	explicit Child(std::unique_ptr<Native> native);

	std::unique_ptr<Native> _native;

};

// A signal that kills a host for a forbidden system call, 0 where there is none.
[[nodiscard]] int SandboxViolationSignal();
void CloseChannel(Handle channel);

} // namespace fgpl::os
