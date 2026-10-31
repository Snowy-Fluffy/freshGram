#include "fgpl/folder.h"

#include <algorithm>
#include <cwctype>
#include <span>
#include <utility>
#include <windows.h>

namespace fgpl::folder {
namespace {

[[nodiscard]] std::wstring Wide(std::string_view text) {
	if (text.empty()) {
		return {};
	}
	const auto size = ::MultiByteToWideChar(
		CP_UTF8,
		MB_ERR_INVALID_CHARS,
		text.data(),
		int(text.size()),
		nullptr,
		0);
	if (size <= 0) {
		return {};
	}
	auto result = std::wstring(size_t(size), L'\0');
	::MultiByteToWideChar(
		CP_UTF8,
		MB_ERR_INVALID_CHARS,
		text.data(),
		int(text.size()),
		result.data(),
		size);
	return result;
}

[[nodiscard]] std::string Narrow(const std::wstring &text) {
	if (text.empty()) {
		return {};
	}
	const auto size = ::WideCharToMultiByte(
		CP_UTF8,
		0,
		text.data(),
		int(text.size()),
		nullptr,
		0,
		nullptr,
		nullptr);
	if (size <= 0) {
		return {};
	}
	auto result = std::string(size_t(size), '\0');
	::WideCharToMultiByte(
		CP_UTF8,
		0,
		text.data(),
		int(text.size()),
		result.data(),
		size,
		nullptr,
		nullptr);
	return result;
}

class Handle {
public:
	Handle() = default;
	explicit Handle(HANDLE handle) : _handle(handle) {
	}
	Handle(Handle &&other) noexcept
	: _handle(std::exchange(other._handle, INVALID_HANDLE_VALUE)) {
	}
	Handle &operator=(Handle &&other) noexcept {
		if (this != &other) {
			reset();
			_handle = std::exchange(other._handle, INVALID_HANDLE_VALUE);
		}
		return *this;
	}
	~Handle() {
		reset();
	}

	[[nodiscard]] HANDLE get() const {
		return _handle;
	}
	[[nodiscard]] bool valid() const {
		return _handle != INVALID_HANDLE_VALUE;
	}

private:
	void reset() {
		if (_handle != INVALID_HANDLE_VALUE) {
			::CloseHandle(_handle);
			_handle = INVALID_HANDLE_VALUE;
		}
	}

	HANDLE _handle = INVALID_HANDLE_VALUE;

};

[[nodiscard]] Fail FromError(DWORD error) {
	switch (error) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND: return Fail::NotFound;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
	case ERROR_CANT_ACCESS_FILE:
	case ERROR_WRITE_PROTECT: return Fail::Denied;
	case ERROR_ALREADY_EXISTS:
	case ERROR_FILE_EXISTS: return Fail::Exists;
	case ERROR_DIR_NOT_EMPTY: return Fail::NotEmpty;
	case ERROR_FILENAME_EXCED_RANGE:
	case ERROR_INVALID_NAME: return Fail::Invalid;
	case ERROR_DISK_FULL:
	case ERROR_HANDLE_DISK_FULL:
	case ERROR_FILE_TOO_LARGE: return Fail::Limit;
	}
	return Fail::Failed;
}

[[nodiscard]] std::wstring Lower(std::wstring text) {
	std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
		return wchar_t(std::towlower(ch));
	});
	return text;
}

[[nodiscard]] std::wstring FinalPath(HANDLE handle) {
	auto buffer = std::wstring(1024, L'\0');
	auto size = ::GetFinalPathNameByHandleW(
		handle,
		buffer.data(),
		DWORD(buffer.size()),
		VOLUME_NAME_DOS);
	if (size >= buffer.size()) {
		buffer.resize(size + 1);
		size = ::GetFinalPathNameByHandleW(
			handle,
			buffer.data(),
			DWORD(buffer.size()),
			VOLUME_NAME_DOS);
	}
	if (size == 0 || size >= buffer.size()) {
		return {};
	}
	buffer.resize(size);
	return Lower(buffer);
}

[[nodiscard]] bool Beneath(const std::wstring &path, const std::wstring &base) {
	return !base.empty()
		&& path.size() > base.size()
		&& path.compare(0, base.size(), base) == 0
		&& path[base.size()] == L'\\';
}

[[nodiscard]] Handle OpenPath(
		const std::wstring &path,
		DWORD access,
		DWORD share,
		DWORD disposition,
		DWORD flags = 0) {
	return Handle(::CreateFileW(
		path.c_str(),
		access,
		share,
		nullptr,
		disposition,
		flags | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
		nullptr));
}

struct Root {
	std::wstring path;
	std::wstring final;
};

[[nodiscard]] Result<Root, Fail> OpenRoot(const std::string &root) {
	auto result = Root();
	result.path = Wide(root);
	if (result.path.empty()) {
		return Fail::Invalid;
	}
	const auto handle = OpenPath(
		result.path,
		FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		OPEN_EXISTING);
	if (!handle.valid()) {
		return FromError(::GetLastError());
	}
	result.final = FinalPath(handle.get());
	if (result.final.empty()) {
		return Fail::Failed;
	}
	return result;
}

// Refuses a link on any directory of the way; the checks on opened handles close the race.
[[nodiscard]] Result<std::wstring, Fail> Build(
		const Root &root,
		const std::vector<std::string> &parts,
		bool leafMayBeMissing) {
	auto path = root.path;
	for (auto i = size_t(0); i != parts.size(); ++i) {
		const auto wide = Wide(parts[i]);
		if (wide.empty()) {
			return Fail::Invalid;
		}
		path += L'\\';
		path += wide;
		const auto attributes = ::GetFileAttributesW(path.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) {
			const auto error = ::GetLastError();
			if (i + 1 == parts.size() && leafMayBeMissing
				&& (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
				return path;
			}
			return FromError(error);
		} else if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
			return Fail::Denied;
		} else if (i + 1 != parts.size() && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
			return Fail::Denied;
		}
	}
	return path;
}

[[nodiscard]] bool InsideRoot(const Handle &handle, const Root &root) {
	return Beneath(FinalPath(handle.get()), root.final);
}

[[nodiscard]] int64_t UnixTime(const FILETIME &time) {
	const auto ticks = (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
	return int64_t(ticks / 10000000ULL) - 11644473600LL;
}

[[nodiscard]] Entry EntryOf(const std::string &name, const WIN32_FILE_ATTRIBUTE_DATA &data) {
	auto result = Entry();
	result.name = name;
	result.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	result.size = result.directory
		? 0
		: ((uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow);
	result.modified = UnixTime(data.ftLastWriteTime);
	return result;
}

} // namespace

Result<std::string, Fail> CheckRoot(const std::string &path) {
	const auto wide = Wide(path);
	if (wide.empty()) {
		return Fail::Invalid;
	}
	const auto handle = OpenPath(
		wide,
		FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		OPEN_EXISTING);
	if (!handle.valid()) {
		return FromError(::GetLastError());
	}
	auto info = BY_HANDLE_FILE_INFORMATION();
	if (!::GetFileInformationByHandle(handle.get(), &info)
		|| !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
		return Fail::Invalid;
	}
	auto final = FinalPath(handle.get());
	if (final.rfind(L"\\\\?\\unc\\", 0) == 0) {
		final = L"\\\\" + final.substr(8);
	} else if (final.rfind(L"\\\\?\\", 0) == 0) {
		final = final.substr(4);
	}
	if (final.size() <= 3) {
		return Fail::Denied;
	}
	const auto env = [](const wchar_t *name) {
		auto buffer = std::wstring(1024, L'\0');
		const auto size = ::GetEnvironmentVariableW(name, buffer.data(), DWORD(buffer.size()));
		if (size == 0 || size >= buffer.size()) {
			return std::wstring();
		}
		buffer.resize(size);
		return Lower(buffer);
	};
	for (const auto name : { L"SystemRoot", L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramData" }) {
		const auto base = env(name);
		if (!base.empty() && (final == base || Beneath(final, base))) {
			return Fail::Denied;
		}
	}
	if (const auto home = env(L"USERPROFILE"); !home.empty()) {
		if (final == home || Beneath(home, final)) {
			return Fail::Denied;
		}
	}
	return Narrow(final);
}

Result<Listing, Fail> List(
		const std::string &root,
		std::string_view path,
		size_t offset,
		size_t limit) {
	const auto parts = SplitPath(path, true);
	if (!parts.ok()) {
		return parts.error();
	}
	const auto opened = OpenRoot(root);
	if (!opened.ok()) {
		return opened.error();
	}
	const auto directory = Build(opened.value(), parts.value(), false);
	if (!directory.ok()) {
		return directory.error();
	}
	auto find = WIN32_FIND_DATAW();
	const auto search = directory.value() + L"\\*";
	const auto first = ::FindFirstFileExW(
		search.c_str(),
		FindExInfoBasic,
		&find,
		FindExSearchNameMatch,
		nullptr,
		0);
	if (first == INVALID_HANDLE_VALUE) {
		return FromError(::GetLastError());
	}
	auto names = std::vector<std::pair<std::string, WIN32_FILE_ATTRIBUTE_DATA>>();
	auto scanned = size_t(0);
	auto failure = false;
	do {
		const auto name = std::wstring(find.cFileName);
		if (name == L"." || name == L"..") {
			continue;
		} else if (++scanned > kMaxScanned) {
			failure = true;
			break;
		} else if (find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
			continue;
		}
		auto data = WIN32_FILE_ATTRIBUTE_DATA();
		data.dwFileAttributes = find.dwFileAttributes;
		data.ftLastWriteTime = find.ftLastWriteTime;
		data.nFileSizeHigh = find.nFileSizeHigh;
		data.nFileSizeLow = find.nFileSizeLow;
		names.emplace_back(Narrow(name), data);
	} while (::FindNextFileW(first, &find));
	::FindClose(first);
	if (failure) {
		return Fail::Limit;
	}
	std::sort(names.begin(), names.end(), [](const auto &a, const auto &b) {
		return a.first < b.first;
	});
	auto result = Listing();
	const auto page = std::min(limit, kMaxListPage);
	auto skipped = size_t(0);
	for (const auto &[name, data] : names) {
		if (skipped < offset) {
			++skipped;
			continue;
		} else if (result.entries.size() >= page) {
			result.more = true;
			break;
		}
		result.entries.push_back(EntryOf(name, data));
	}
	return result;
}

Result<Entry, Fail> Stat(const std::string &root, std::string_view path) {
	const auto parts = SplitPath(path, false);
	if (!parts.ok()) {
		return parts.error();
	}
	const auto opened = OpenRoot(root);
	if (!opened.ok()) {
		return opened.error();
	}
	const auto target = Build(opened.value(), parts.value(), false);
	if (!target.ok()) {
		return target.error();
	}
	auto data = WIN32_FILE_ATTRIBUTE_DATA();
	if (!::GetFileAttributesExW(target.value().c_str(), GetFileExInfoStandard, &data)) {
		return FromError(::GetLastError());
	}
	return EntryOf(parts.value().back(), data);
}

Result<std::vector<uint8_t>, Fail> Read(
		const std::string &root,
		std::string_view path,
		uint64_t offset,
		size_t length) {
	const auto parts = SplitPath(path, false);
	if (!parts.ok()) {
		return parts.error();
	}
	const auto opened = OpenRoot(root);
	if (!opened.ok()) {
		return opened.error();
	}
	const auto target = Build(opened.value(), parts.value(), false);
	if (!target.ok()) {
		return target.error();
	}
	const auto file = OpenPath(
		target.value(),
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		OPEN_EXISTING);
	if (!file.valid()) {
		return FromError(::GetLastError());
	}
	auto info = BY_HANDLE_FILE_INFORMATION();
	if (!::GetFileInformationByHandle(file.get(), &info)
		|| (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
		|| !InsideRoot(file, opened.value())) {
		return Fail::Denied;
	}
	const auto size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
	if (offset >= size) {
		return std::vector<uint8_t>();
	}
	const auto count = size_t(std::min<uint64_t>(length, size - offset));
	auto position = LARGE_INTEGER();
	position.QuadPart = LONGLONG(offset);
	if (!::SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN)) {
		return FromError(::GetLastError());
	}
	auto bytes = std::vector<uint8_t>(count);
	auto done = size_t(0);
	while (done < count) {
		auto got = DWORD(0);
		const auto chunk = DWORD(std::min<size_t>(count - done, 1U << 20));
		if (!::ReadFile(file.get(), bytes.data() + done, chunk, &got, nullptr) || got == 0) {
			break;
		}
		done += got;
	}
	bytes.resize(done);
	return bytes;
}

Result<bool, Fail> Write(
		const std::string &root,
		std::string_view path,
		std::span<const uint8_t> bytes,
		WriteMode mode) {
	const auto parts = SplitPath(path, false);
	if (!parts.ok()) {
		return parts.error();
	}
	const auto opened = OpenRoot(root);
	if (!opened.ok()) {
		return opened.error();
	}
	const auto target = Build(opened.value(), parts.value(), true);
	if (!target.ok()) {
		return target.error();
	}
	const auto exists = (::GetFileAttributesW(target.value().c_str()) != INVALID_FILE_ATTRIBUTES);
	if (mode == WriteMode::Create && exists) {
		return Fail::Exists;
	}
	const auto file = OpenPath(
		target.value(),
		GENERIC_WRITE,
		FILE_SHARE_READ,
		exists ? OPEN_EXISTING : CREATE_NEW);
	if (!file.valid()) {
		return FromError(::GetLastError());
	}
	auto info = BY_HANDLE_FILE_INFORMATION();
	if (!::GetFileInformationByHandle(file.get(), &info)
		|| (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
		|| (exists && info.nNumberOfLinks > 1)
		|| !InsideRoot(file, opened.value())) {
		return Fail::Denied;
	}
	if (mode == WriteMode::Replace && exists && !::SetEndOfFile(file.get())) {
		return FromError(::GetLastError());
	}
	auto position = LARGE_INTEGER();
	if (mode == WriteMode::Append) {
		position.QuadPart = (LONGLONG(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
	}
	if (!::SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN)) {
		return FromError(::GetLastError());
	}
	auto done = size_t(0);
	while (done < bytes.size()) {
		auto put = DWORD(0);
		const auto chunk = DWORD(std::min<size_t>(bytes.size() - done, 1U << 20));
		if (!::WriteFile(file.get(), bytes.data() + done, chunk, &put, nullptr) || put == 0) {
			return FromError(::GetLastError());
		}
		done += put;
	}
	return true;
}

Result<bool, Fail> MakeDirectory(
		const std::string &root,
		std::string_view path) {
	const auto parts = SplitPath(path, false);
	if (!parts.ok()) {
		return parts.error();
	}
	const auto opened = OpenRoot(root);
	if (!opened.ok()) {
		return opened.error();
	}
	const auto target = Build(opened.value(), parts.value(), true);
	if (!target.ok()) {
		return target.error();
	}
	if (::CreateDirectoryW(target.value().c_str(), nullptr)) {
		return true;
	}
	const auto error = ::GetLastError();
	if (error == ERROR_ALREADY_EXISTS) {
		const auto attributes = ::GetFileAttributesW(target.value().c_str());
		if (attributes != INVALID_FILE_ATTRIBUTES
			&& (attributes & FILE_ATTRIBUTE_DIRECTORY)
			&& !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
			return true;
		}
	}
	return FromError(error);
}

Result<bool, Fail> Remove(const std::string &root, std::string_view path) {
	const auto parts = SplitPath(path, false);
	if (!parts.ok()) {
		return parts.error();
	}
	const auto opened = OpenRoot(root);
	if (!opened.ok()) {
		return opened.error();
	}
	const auto target = Build(opened.value(), parts.value(), false);
	if (!target.ok()) {
		return target.error();
	}
	const auto attributes = ::GetFileAttributesW(target.value().c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		return FromError(::GetLastError());
	}
	const auto removed = (attributes & FILE_ATTRIBUTE_DIRECTORY)
		? ::RemoveDirectoryW(target.value().c_str())
		: ::DeleteFileW(target.value().c_str());
	if (!removed) {
		return FromError(::GetLastError());
	}
	return true;
}

Result<bool, Fail> Rename(
		const std::string &root,
		std::string_view from,
		std::string_view to) {
	const auto source = SplitPath(from, false);
	const auto destination = SplitPath(to, false);
	if (!source.ok()) {
		return source.error();
	} else if (!destination.ok()) {
		return destination.error();
	}
	const auto opened = OpenRoot(root);
	if (!opened.ok()) {
		return opened.error();
	}
	const auto sourcePath = Build(opened.value(), source.value(), false);
	if (!sourcePath.ok()) {
		return sourcePath.error();
	}
	const auto destinationPath = Build(opened.value(), destination.value(), true);
	if (!destinationPath.ok()) {
		return destinationPath.error();
	}
	if (::GetFileAttributesW(destinationPath.value().c_str()) != INVALID_FILE_ATTRIBUTES) {
		return Fail::Exists;
	} else if (!::MoveFileExW(sourcePath.value().c_str(), destinationPath.value().c_str(), 0)) {
		return FromError(::GetLastError());
	}
	return true;
}

} // namespace fgpl::folder
