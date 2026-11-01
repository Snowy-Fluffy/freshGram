#include "fgpl/folder.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <span>
#include <utility>

namespace fgpl::folder {
namespace {

using StatBuf = struct stat;

class Fd {
public:
	Fd() = default;
	explicit Fd(int fd) : _fd(fd) {
	}
	Fd(Fd &&other) noexcept : _fd(std::exchange(other._fd, -1)) {
	}
	Fd &operator=(Fd &&other) noexcept {
		if (this != &other) {
			reset();
			_fd = std::exchange(other._fd, -1);
		}
		return *this;
	}
	~Fd() {
		reset();
	}

	[[nodiscard]] int get() const {
		return _fd;
	}
	[[nodiscard]] bool valid() const {
		return _fd >= 0;
	}
	int release() {
		return std::exchange(_fd, -1);
	}

private:
	void reset() {
		if (_fd >= 0) {
			::close(_fd);
			_fd = -1;
		}
	}

	int _fd = -1;

};

constexpr auto kDirFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

[[nodiscard]] Fail FromErrno(int error) {
	switch (error) {
	case ENOENT: return Fail::NotFound;
	case ELOOP:
	case EACCES:
	case EPERM:
	case EXDEV:
	case EROFS:
	case ENOTDIR:
	case EISDIR: return Fail::Denied;
	case EEXIST: return Fail::Exists;
	case ENOTEMPTY: return Fail::NotEmpty;
	case ENAMETOOLONG:
	case EINVAL: return Fail::Invalid;
	case ENOSPC:
	case EDQUOT:
	case EFBIG: return Fail::Limit;
	}
	return Fail::Failed;
}

[[nodiscard]] Fd OpenRoot(const std::string &root) {
	return Fd(::open(root.c_str(), kDirFlags));
}

// Walks the parent directories one component at a time, so no link on the way is ever followed.
[[nodiscard]] Result<Fd, Fail> OpenParent(
		const std::string &root,
		const std::vector<std::string> &parts) {
	auto current = OpenRoot(root);
	if (!current.valid()) {
		return FromErrno(errno);
	}
	for (auto i = size_t(0); i + 1 < parts.size(); ++i) {
		auto next = Fd(::openat(current.get(), parts[i].c_str(), kDirFlags));
		if (!next.valid()) {
			return FromErrno(errno);
		}
		current = std::move(next);
	}
	return Result<Fd, Fail>(std::move(current));
}

[[nodiscard]] Entry EntryOf(const std::string &name, const StatBuf &info) {
	auto result = Entry();
	result.name = name;
	result.directory = S_ISDIR(info.st_mode);
	result.size = result.directory ? 0 : uint64_t(info.st_size);
	result.modified = int64_t(info.st_mtime);
	return result;
}

[[nodiscard]] bool Plain(const StatBuf &info) {
	return S_ISREG(info.st_mode) || S_ISDIR(info.st_mode);
}

struct Located {
	Fd parent;
	std::string leaf;
};

[[nodiscard]] Result<Located, Fail> Locate(
		const std::string &root,
		std::string_view path) {
	const auto parts = SplitPath(path, false);
	if (!parts.ok()) {
		return parts.error();
	}
	auto parent = OpenParent(root, parts.value());
	if (!parent.ok()) {
		return parent.error();
	}
	return Located{ std::move(parent.value()), parts.value().back() };
}

} // namespace

Result<std::string, Fail> CheckRoot(const std::string &path) {
	if (path.empty() || path.front() != '/') {
		return Fail::Invalid;
	}
	char resolved[PATH_MAX];
	if (!::realpath(path.c_str(), resolved)) {
		return FromErrno(errno);
	}
	const auto canonical = std::string(resolved);
	auto info = StatBuf();
	if (::stat(canonical.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
		return Fail::Invalid;
	} else if (::access(canonical.c_str(), R_OK | X_OK) != 0) {
		return Fail::Denied;
	}
	static const auto kExact = {
		"/", "/home", "/Users", "/mnt", "/media", "/Volumes", "/tmp", "/var", "/opt",
		"/private", "/private/var", "/private/tmp", "/Applications", "/srv",
	};
	static const auto kTrees = {
		"/bin", "/boot", "/dev", "/etc", "/lib", "/lib32", "/lib64", "/proc", "/root",
		"/run", "/sbin", "/sys", "/usr", "/System", "/Library", "/private/etc",
	};
	const auto inside = [](std::string_view path, std::string_view base) {
		return (path.size() > base.size())
			&& (path.compare(0, base.size(), base) == 0)
			&& (path[base.size()] == '/');
	};
	for (const auto base : kExact) {
		if (canonical == base) {
			return Fail::Denied;
		}
	}
	for (const auto base : kTrees) {
		if (canonical == base || inside(canonical, base)) {
			return Fail::Denied;
		}
	}
	if (const auto home = std::getenv("HOME"); home && *home) {
		char resolvedHome[PATH_MAX];
		if (::realpath(home, resolvedHome)) {
			const auto own = std::string(resolvedHome);
			if (canonical == own || inside(own, canonical)) {
				return Fail::Denied;
			}
		}
	}
	return canonical;
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
	auto directory = OpenRoot(root);
	if (!directory.valid()) {
		return FromErrno(errno);
	}
	for (const auto &part : parts.value()) {
		auto next = Fd(::openat(directory.get(), part.c_str(), kDirFlags));
		if (!next.valid()) {
			return FromErrno(errno);
		}
		directory = std::move(next);
	}
	const auto stream = ::fdopendir(directory.get());
	if (!stream) {
		return FromErrno(errno);
	}
	directory.release();
	auto names = std::vector<std::string>();
	auto scanned = size_t(0);
	while (const auto item = ::readdir(stream)) {
		const auto name = std::string_view(item->d_name);
		if (name == "." || name == "..") {
			continue;
		} else if (++scanned > kMaxScanned) {
			::closedir(stream);
			return Fail::Limit;
		}
		names.emplace_back(name);
	}
	std::sort(names.begin(), names.end());
	auto result = Listing();
	const auto page = std::min(limit, kMaxListPage);
	const auto fd = ::dirfd(stream);
	auto skipped = size_t(0);
	for (const auto &name : names) {
		auto info = StatBuf();
		if (::fstatat(fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0
			|| !Plain(info)) {
			continue;
		} else if (skipped < offset) {
			++skipped;
			continue;
		} else if (result.entries.size() >= page) {
			result.more = true;
			break;
		}
		result.entries.push_back(EntryOf(name, info));
	}
	::closedir(stream);
	return result;
}

Result<Entry, Fail> Stat(const std::string &root, std::string_view path) {
	auto located = Locate(root, path);
	if (!located.ok()) {
		return located.error();
	}
	auto info = StatBuf();
	if (::fstatat(
			located.value().parent.get(),
			located.value().leaf.c_str(),
			&info,
			AT_SYMLINK_NOFOLLOW) != 0) {
		return FromErrno(errno);
	} else if (!Plain(info)) {
		return Fail::Denied;
	}
	return EntryOf(located.value().leaf, info);
}

Result<std::vector<uint8_t>, Fail> Read(
		const std::string &root,
		std::string_view path,
		uint64_t offset,
		size_t length) {
	auto located = Locate(root, path);
	if (!located.ok()) {
		return located.error();
	}
	const auto file = Fd(::openat(
		located.value().parent.get(),
		located.value().leaf.c_str(),
		O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
	if (!file.valid()) {
		return FromErrno(errno);
	}
	auto info = StatBuf();
	if (::fstat(file.get(), &info) != 0) {
		return FromErrno(errno);
	} else if (!S_ISREG(info.st_mode)) {
		return Fail::Denied;
	}
	const auto size = uint64_t(info.st_size);
	if (offset >= size) {
		return std::vector<uint8_t>();
	}
	const auto count = size_t(std::min<uint64_t>(length, size - offset));
	auto bytes = std::vector<uint8_t>(count);
	auto done = size_t(0);
	while (done < count) {
		const auto got = ::pread(
			file.get(),
			bytes.data() + done,
			count - done,
			off_t(offset + done));
		if (got < 0 && errno == EINTR) {
			continue;
		} else if (got <= 0) {
			break;
		}
		done += size_t(got);
	}
	bytes.resize(done);
	return bytes;
}

Result<bool, Fail> Write(
		const std::string &root,
		std::string_view path,
		std::span<const uint8_t> bytes,
		WriteMode mode) {
	auto located = Locate(root, path);
	if (!located.ok()) {
		return located.error();
	}
	const auto parent = located.value().parent.get();
	const auto leaf = located.value().leaf.c_str();
	auto fresh = false;
	auto file = Fd();
	if (mode == WriteMode::Create) {
		file = Fd(::openat(
			parent,
			leaf,
			O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
			0600));
		fresh = true;
	} else {
		file = Fd(::openat(
			parent,
			leaf,
			O_WRONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
		if (!file.valid() && errno == ENOENT) {
			file = Fd(::openat(
				parent,
				leaf,
				O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
				0600));
			fresh = true;
		}
	}
	if (!file.valid()) {
		return FromErrno(errno);
	}
	auto info = StatBuf();
	if (::fstat(file.get(), &info) != 0) {
		return FromErrno(errno);
	} else if (!S_ISREG(info.st_mode) || (!fresh && info.st_nlink > 1)) {
		return Fail::Denied;
	}
	if (mode == WriteMode::Replace && !fresh && ::ftruncate(file.get(), 0) != 0) {
		return FromErrno(errno);
	}
	auto done = size_t(0);
	auto position = (mode == WriteMode::Append) ? uint64_t(info.st_size) : uint64_t(0);
	while (done < bytes.size()) {
		const auto put = ::pwrite(
			file.get(),
			bytes.data() + done,
			bytes.size() - done,
			off_t(position + done));
		if (put < 0 && errno == EINTR) {
			continue;
		} else if (put <= 0) {
			return FromErrno(errno);
		}
		done += size_t(put);
	}
	return true;
}

Result<bool, Fail> MakeDirectory(
		const std::string &root,
		std::string_view path) {
	auto located = Locate(root, path);
	if (!located.ok()) {
		return located.error();
	}
	if (::mkdirat(located.value().parent.get(), located.value().leaf.c_str(), 0700) == 0) {
		return true;
	} else if (errno == EEXIST) {
		auto info = StatBuf();
		if (::fstatat(
				located.value().parent.get(),
				located.value().leaf.c_str(),
				&info,
				AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(info.st_mode)) {
			return true;
		}
	}
	return FromErrno(errno);
}

Result<bool, Fail> Remove(const std::string &root, std::string_view path) {
	auto located = Locate(root, path);
	if (!located.ok()) {
		return located.error();
	}
	const auto parent = located.value().parent.get();
	const auto leaf = located.value().leaf.c_str();
	auto info = StatBuf();
	if (::fstatat(parent, leaf, &info, AT_SYMLINK_NOFOLLOW) != 0) {
		return FromErrno(errno);
	} else if (!Plain(info)) {
		return Fail::Denied;
	} else if (::unlinkat(parent, leaf, S_ISDIR(info.st_mode) ? AT_REMOVEDIR : 0) != 0) {
		return FromErrno(errno);
	}
	return true;
}

Result<bool, Fail> Rename(
		const std::string &root,
		std::string_view from,
		std::string_view to) {
	auto source = Locate(root, from);
	if (!source.ok()) {
		return source.error();
	}
	auto target = Locate(root, to);
	if (!target.ok()) {
		return target.error();
	}
	auto info = StatBuf();
	if (::fstatat(
			source.value().parent.get(),
			source.value().leaf.c_str(),
			&info,
			AT_SYMLINK_NOFOLLOW) != 0) {
		return FromErrno(errno);
	} else if (!Plain(info)) {
		return Fail::Denied;
	} else if (::fstatat(
			target.value().parent.get(),
			target.value().leaf.c_str(),
			&info,
			AT_SYMLINK_NOFOLLOW) == 0) {
		return Fail::Exists;
	}
	if (::renameat(
			source.value().parent.get(),
			source.value().leaf.c_str(),
			target.value().parent.get(),
			target.value().leaf.c_str()) != 0) {
		return FromErrno(errno);
	}
	return true;
}

} // namespace fgpl::folder
