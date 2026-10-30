#include "fgpl/os.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <sys/random.h>

#ifdef __APPLE__
#include <libproc.h>
#include <mach/mach_time.h>
#include <sys/proc_info.h>
#endif // __APPLE__

namespace fgpl::os {

int ProcessId() {
	return int(getpid());
}

void RandomBytes(std::span<uint8_t> out) {
	auto offset = size_t(0);
	while (offset < out.size()) {
#ifdef __APPLE__
		const auto step = std::min(out.size() - offset, size_t(256));
		if (getentropy(out.data() + offset, step) != 0) {
			std::abort();
		}
		offset += step;
#else // __APPLE__
		const auto got = getrandom(out.data() + offset, out.size() - offset, 0);
		if (got < 0 && errno == EINTR) {
			continue;
		} else if (got <= 0) {
			std::abort();
		}
		offset += size_t(got);
#endif // __APPLE__
	}
}

std::filesystem::path PathFromUtf8(std::string_view text) {
	return std::filesystem::path(std::string(text));
}

std::string PathToUtf8(const std::filesystem::path &path) {
	return path.string();
}

bool WriteDurable(
		const std::filesystem::path &file,
		std::span<const uint8_t> bytes) {
	const auto fd = ::open(
		file.c_str(),
		O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
		0600);
	if (fd < 0) {
		return false;
	}
	auto written = size_t(0);
	while (written < bytes.size()) {
		const auto result = ::write(
			fd,
			bytes.data() + written,
			bytes.size() - written);
		if (result < 0 && errno == EINTR) {
			continue;
		} else if (result <= 0) {
			::close(fd);
			return false;
		}
		written += size_t(result);
	}
#ifdef __APPLE__
	// fsync leaves the data in the drive cache on macOS.
	const auto synced = (::fcntl(fd, F_FULLFSYNC) == 0) || (::fsync(fd) == 0);
#else // __APPLE__
	const auto synced = (::fsync(fd) == 0);
#endif // __APPLE__
	return (::close(fd) == 0) && synced;
}

void SyncDirectory(const std::filesystem::path &directory) {
	const auto fd = ::open(
		directory.c_str(),
		O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		::fsync(fd);
		::close(fd);
	}
}

bool ReplaceFileAtomic(
		const std::filesystem::path &from,
		const std::filesystem::path &to,
		std::error_code &error) {
	std::filesystem::rename(from, to, error);
	return !error;
}

void MakeExecutable(const std::filesystem::path &file) {
	::chmod(file.c_str(), 0700);
}

bool SetCurrent(const std::filesystem::path &root, const std::string &name) {
	auto error = std::error_code();
	const auto temporary = root / ("current.tmp-" + std::to_string(ProcessId()));
	std::filesystem::remove(temporary, error);
	std::filesystem::create_directory_symlink(name, temporary, error);
	if (!error) {
		std::filesystem::rename(temporary, root / "current", error);
	}
	if (error) {
		std::filesystem::remove(temporary, error);
		return false;
	}
	return true;
}

std::filesystem::path ResolveCurrent(const std::filesystem::path &root) {
	return root / "current";
}

Usage ProcessUsage(int pid) {
	if (pid <= 0) {
		return {};
	}
#ifdef __APPLE__
	auto info = proc_taskinfo();
	if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &info, sizeof(info)) != int(sizeof(info))) {
		return {};
	}
	auto timebase = mach_timebase_info_data_t();
	mach_timebase_info(&timebase);
	const auto ticks = info.pti_total_user + info.pti_total_system;
	const auto nanoseconds = (timebase.denom != 0)
		? ticks * timebase.numer / timebase.denom
		: ticks;
	return { nanoseconds / 1000000, info.pti_resident_size };
#else // __APPLE__
	auto stream = std::ifstream("/proc/" + std::to_string(pid) + "/stat");
	auto line = std::string();
	if (!std::getline(stream, line)) {
		return {};
	}
	const auto close = line.rfind(')');
	if (close == std::string::npos) {
		return {};
	}
	auto fields = std::istringstream(line.substr(close + 2));
	auto values = std::vector<std::string>();
	for (auto value = std::string(); fields >> value;) {
		values.push_back(value);
	}
	if (values.size() < 22) {
		return {};
	}
	const auto ticks = sysconf(_SC_CLK_TCK);
	const auto page = sysconf(_SC_PAGESIZE);
	const auto cpu = std::stoull(values[11]) + std::stoull(values[12]);
	return {
		ticks > 0 ? cpu * 1000 / uint64_t(ticks) : 0,
		std::stoull(values[21]) * uint64_t(page > 0 ? page : 4096),
	};
#endif // __APPLE__
}

void CloseChannel(Handle channel) {
	if (channel >= 0) {
		::close(int(channel));
	}
}

} // namespace fgpl::os
