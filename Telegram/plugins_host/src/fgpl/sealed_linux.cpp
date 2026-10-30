#include "fgpl/os.h"

#include "fgpl/package.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace fgpl::os {
namespace {

constexpr auto kMaxExecutableBytes = off_t(256) << 20;

#ifndef MFD_EXEC
#define MFD_EXEC 0x0010U
#endif

[[nodiscard]] bool MemfdExecAllowed() {
	auto stream = open("/proc/sys/vm/memfd_noexec", O_RDONLY | O_CLOEXEC);
	if (stream < 0) {
		return true;
	}
	auto value = char();
	const auto read = ::read(stream, &value, 1);
	close(stream);
	return read != 1 || value != '2';
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
	if (HashText(data) != hash) {
		close(file);
		return std::string("the host does not match the signed runtime index");
	}
	if (!MemfdExecAllowed()) {
		return Handle(file);
	}
	auto sealed = memfd_create("fgpl_plugin_host", MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_EXEC);
	if (sealed < 0 && errno == EINVAL) {
		sealed = memfd_create("fgpl_plugin_host", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	}
	if (sealed < 0) {
		return Handle(file);
	}
	const auto seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
	if (!WriteAll(sealed, data) || fcntl(sealed, F_ADD_SEALS, seals) != 0) {
		close(sealed);
		return Handle(file);
	}
	close(file);
	return Handle(sealed);
}

void CloseExecutable(Handle handle) {
	if (handle >= 0) {
		close(int(handle));
	}
}

} // namespace fgpl::os
