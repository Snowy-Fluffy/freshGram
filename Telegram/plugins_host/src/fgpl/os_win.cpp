#include "fgpl/os.h"

#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>

namespace fgpl::os {
namespace {

std::mutex SendTimeoutsMutex;
std::map<Handle, int> SendTimeouts;

[[nodiscard]] HANDLE Native(Handle handle) {
	return reinterpret_cast<HANDLE>(handle);
}

[[nodiscard]] std::error_code LastError() {
	return std::error_code(int(GetLastError()), std::system_category());
}

} // namespace

void SetSendTimeout(Handle channel, int milliseconds) {
	const auto lock = std::scoped_lock(SendTimeoutsMutex);
	SendTimeouts[channel] = milliseconds;
}

thread_local int LastChannelError = 0;

void NoteChannelError(int code) {
	LastChannelError = code;
}

int ChannelError() {
	return LastChannelError;
}

int SendTimeout(Handle channel) {
	const auto lock = std::scoped_lock(SendTimeoutsMutex);
	const auto i = SendTimeouts.find(channel);
	return (i != SendTimeouts.end()) ? i->second : 0;
}

int ProcessId() {
	return int(GetCurrentProcessId());
}

void RandomBytes(std::span<uint8_t> out) {
	auto offset = size_t(0);
	while (offset < out.size()) {
		const auto step = ULONG(std::min<size_t>(out.size() - offset, 1 << 20));
		if (BCryptGenRandom(
				nullptr,
				out.data() + offset,
				step,
				BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
			std::abort();
		}
		offset += step;
	}
}

std::filesystem::path PathFromUtf8(std::string_view text) {
	if (text.empty()) {
		return {};
	}
	const auto size = MultiByteToWideChar(
		CP_UTF8,
		0,
		text.data(),
		int(text.size()),
		nullptr,
		0);
	auto wide = std::wstring(size_t(std::max(size, 0)), L'\0');
	if (size > 0) {
		MultiByteToWideChar(
			CP_UTF8,
			0,
			text.data(),
			int(text.size()),
			wide.data(),
			size);
	}
	return std::filesystem::path(std::move(wide));
}

std::string PathToUtf8(const std::filesystem::path &path) {
	const auto &wide = path.native();
	if (wide.empty()) {
		return {};
	}
	const auto size = WideCharToMultiByte(
		CP_UTF8,
		0,
		wide.data(),
		int(wide.size()),
		nullptr,
		0,
		nullptr,
		nullptr);
	auto result = std::string(size_t(std::max(size, 0)), '\0');
	if (size > 0) {
		WideCharToMultiByte(
			CP_UTF8,
			0,
			wide.data(),
			int(wide.size()),
			result.data(),
			size,
			nullptr,
			nullptr);
	}
	return result;
}

bool WriteDurable(
		const std::filesystem::path &file,
		std::span<const uint8_t> bytes) {
	const auto handle = CreateFileW(
		file.c_str(),
		GENERIC_WRITE,
		0,
		nullptr,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return false;
	}
	auto written = size_t(0);
	while (written < bytes.size()) {
		const auto step = DWORD(std::min<size_t>(bytes.size() - written, 1 << 20));
		auto done = DWORD(0);
		if (!WriteFile(handle, bytes.data() + written, step, &done, nullptr)
			|| done == 0) {
			CloseHandle(handle);
			return false;
		}
		written += done;
	}
	const auto synced = FlushFileBuffers(handle) != FALSE;
	return (CloseHandle(handle) != FALSE) && synced;
}

void SyncDirectory(const std::filesystem::path &) {
}

bool ReplaceFileAtomic(
		const std::filesystem::path &from,
		const std::filesystem::path &to,
		std::error_code &error) {
	error.clear();
	// Antivirus scanners and indexers hold fresh files for a moment.
	for (auto attempt = 0; attempt != 6; ++attempt) {
		if (MoveFileExW(
				from.c_str(),
				to.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			return true;
		}
		const auto code = GetLastError();
		if (code != ERROR_ACCESS_DENIED
			&& code != ERROR_SHARING_VIOLATION
			&& code != ERROR_LOCK_VIOLATION) {
			break;
		}
		Sleep(DWORD(20 * (attempt + 1)));
	}
	error = LastError();
	return false;
}

void MakeExecutable(const std::filesystem::path &) {
}

bool SetCurrent(const std::filesystem::path &root, const std::string &name) {
	// Creating symlinks needs a privilege, a pointer file does not.
	const auto temporary = root / ("current.tmp-" + std::to_string(ProcessId()));
	const auto bytes = std::vector<uint8_t>(name.begin(), name.end());
	if (!WriteDurable(temporary, bytes)) {
		return false;
	}
	auto error = std::error_code();
	if (!ReplaceFileAtomic(temporary, root / "current", error)) {
		std::filesystem::remove(temporary, error);
		return false;
	}
	return true;
}

std::filesystem::path ResolveCurrent(const std::filesystem::path &root) {
	auto error = std::error_code();
	const auto pointer = root / "current";
	if (!std::filesystem::is_regular_file(pointer, error)) {
		return pointer;
	}
	auto stream = std::ifstream(pointer, std::ios::binary);
	auto name = std::string();
	if (!std::getline(stream, name)
		|| name.rfind("rev-", 0) != 0
		|| name.find_first_of("/\\:") != std::string::npos) {
		return pointer;
	}
	return root / PathFromUtf8(name);
}

Usage ProcessUsage(int pid) {
	if (pid <= 0) {
		return {};
	}
	const auto process = OpenProcess(
		PROCESS_QUERY_LIMITED_INFORMATION,
		FALSE,
		DWORD(pid));
	if (!process) {
		return {};
	}
	auto result = Usage();
	FILETIME created, exited, kernel, user;
	if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
		const auto sum = [](const FILETIME &time) {
			return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
		};
		// FILETIME counts 100 ns.
		result.cpuMs = (sum(kernel) + sum(user)) / 10000;
	}
	auto counters = PROCESS_MEMORY_COUNTERS();
	if (GetProcessMemoryInfo(process, &counters, sizeof(counters))) {
		result.rssBytes = counters.WorkingSetSize;
	}
	CloseHandle(process);
	return result;
}

void CloseChannel(Handle channel) {
	if (channel == kNoHandle || channel == 0) {
		return;
	}
	{
		const auto lock = std::scoped_lock(SendTimeoutsMutex);
		SendTimeouts.erase(channel);
	}
	CloseHandle(Native(channel));
}

} // namespace fgpl::os
