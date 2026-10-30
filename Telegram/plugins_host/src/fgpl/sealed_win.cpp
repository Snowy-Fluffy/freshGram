#include "fgpl/os.h"

#include "fgpl/package.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <vector>

namespace fgpl::os {
namespace {

constexpr auto kMaxExecutableBytes = uint64_t(256) << 20;

} // namespace

// There is no memfd here. The file stays open without write and delete
// sharing, which keeps it, and the directories above it, from changing.
Result<Handle, std::string> SealExecutable(
		const std::string &path,
		const std::string &hash) {
	const auto file = CreateFileW(
		PathFromUtf8(path).c_str(),
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return "cannot open the host (" + std::to_string(GetLastError()) + ")";
	}
	auto info = BY_HANDLE_FILE_INFORMATION();
	if (!GetFileInformationByHandle(file, &info)) {
		CloseHandle(file);
		return std::string("cannot inspect the host");
	}
	const auto size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
	if ((info.dwFileAttributes
			& (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
		|| size == 0
		|| size > kMaxExecutableBytes) {
		CloseHandle(file);
		return std::string("the host is not a regular file of a sane size");
	}
	auto data = std::vector<uint8_t>(size_t(size));
	auto offset = size_t(0);
	while (offset < data.size()) {
		auto got = DWORD(0);
		const auto step = DWORD(std::min<size_t>(data.size() - offset, 1 << 24));
		if (!ReadFile(file, data.data() + offset, step, &got, nullptr)
			|| got == 0) {
			CloseHandle(file);
			return std::string("cannot read the host");
		}
		offset += got;
	}
	if (HashText(data) != hash) {
		CloseHandle(file);
		return std::string("the host does not match the signed runtime index");
	}
	return reinterpret_cast<Handle>(file);
}

void CloseExecutable(Handle handle) {
	if (handle != kNoHandle && handle != 0) {
		CloseHandle(reinterpret_cast<HANDLE>(handle));
	}
}

} // namespace fgpl::os
