#include "fgpl/os.h"

#include "fgpl/package.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace fgpl::os {
namespace {

constexpr auto kMaxExecutableBytes = off_t(256) << 20;

[[nodiscard]] std::string PrivateDirectoryBase() {
	// The per-user temporary directory is closed to everyone else.
	char buffer[1024] = {};
	if (confstr(_CS_DARWIN_USER_TEMP_DIR, buffer, sizeof(buffer)) > 0) {
		return buffer;
	}
	return "/tmp/";
}

[[nodiscard]] bool WriteAll(int fd, const std::vector<uint8_t> &data) {
	auto offset = size_t(0);
	while (offset < data.size()) {
		const auto written = write(fd, data.data() + offset, data.size() - offset);
		if (written < 0 && errno == EINTR) {
			continue;
		} else if (written <= 0) {
			return false;
		}
		offset += size_t(written);
	}
	return true;
}

} // namespace

// There is no fexecve here, so the verified bytes go into a fresh directory
// that only this user can enter and the host starts from that copy. What is
// left is a race against a process of the same user, which can edit the
// whole profile anyway.
Result<Handle, std::string> SealExecutable(
		const std::string &path,
		const std::string &hash) {
	const auto file = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (file < 0) {
		return std::string("cannot open the host: ") + std::strerror(errno);
	}
	struct stat info {};
	if (fstat(file, &info) != 0 || !S_ISREG(info.st_mode)
		|| info.st_size <= 0 || info.st_size > kMaxExecutableBytes) {
		close(file);
		return std::string("the host is not a regular file of a sane size");
	}
	auto data = std::vector<uint8_t>(size_t(info.st_size));
	auto offset = size_t(0);
	while (offset < data.size()) {
		const auto got = pread(file, data.data() + offset, data.size() - offset, off_t(offset));
		if (got < 0 && errno == EINTR) {
			continue;
		} else if (got <= 0) {
			close(file);
			return std::string("cannot read the host");
		}
		offset += size_t(got);
	}
	close(file);
	if (HashText(data) != hash) {
		return std::string("the host does not match the signed runtime index");
	}
	auto directory = PrivateDirectoryBase() + "fgpl-host-XXXXXX";
	if (!mkdtemp(directory.data())) {
		return std::string("cannot make a private directory: ") + std::strerror(errno);
	}
	const auto copy = directory + "/" + kHostFileName;
	const auto fail = [&](const char *what) {
		unlink(copy.c_str());
		rmdir(directory.c_str());
		return std::string(what) + ": " + std::strerror(errno);
	};
	const auto writer = open(
		copy.c_str(),
		O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
		0700);
	if (writer < 0) {
		return fail("cannot create the host copy");
	}
	const auto written = WriteAll(writer, data)
		&& fchmod(writer, 0500) == 0;
	const auto saved = errno;
	close(writer);
	if (!written) {
		errno = saved;
		return fail("cannot write the host copy");
	}
	const auto reader = open(copy.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (reader < 0) {
		return fail("cannot reopen the host copy");
	}
	return Handle(reader);
}

void CloseExecutable(Handle handle) {
	if (handle < 0) {
		return;
	}
	char path[1024] = {};
	if (fcntl(int(handle), F_GETPATH, path) == 0) {
		auto copy = std::string(path);
		unlink(copy.c_str());
		const auto slash = copy.rfind('/');
		if (slash != std::string::npos) {
			copy.resize(slash);
			rmdir(copy.c_str());
		}
	}
	close(int(handle));
}

} // namespace fgpl::os
