#include "fgpl/sandbox.h"

#include <fcntl.h>
#include <sandbox.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace fgpl::sandbox {
namespace {

// Nothing but the pipe the host already holds, its own threads and signals.
constexpr auto kProfile = R"((version 1)
(deny default)
(allow signal (target self))
(allow process-info-pidinfo (target self))
(allow sysctl-read
	(sysctl-name
		"hw.pagesize"
		"hw.ncpu"
		"hw.activecpu"
		"hw.logicalcpu"
		"hw.physicalcpu"
		"hw.memsize"))
)";

[[nodiscard]] std::string Errno(const char *what) {
	return std::string(what) + ": " + std::strerror(errno);
}

void CloseExtraDescriptors(int keep) {
	const auto limit = std::min(getdtablesize(), 4096);
	for (auto fd = 3; fd < limit; ++fd) {
		if (fd != keep) {
			close(fd);
		}
	}
}

[[nodiscard]] bool SetLimit(int resource, rlim_t value) {
	const auto limit = rlimit{ .rlim_cur = value, .rlim_max = value };
	return setrlimit(resource, &limit) == 0;
}

} // namespace

bool Supported() {
	return true;
}

Result<Report, std::string> Restrict(const Options &options, Report report) {
	// A debugger could rewrite the sandbox away.
	ptrace(PT_DENY_ATTACH, 0, nullptr, 0);
	if (!options.layers) {
		return report;
	}
	CloseExtraDescriptors(int(options.ipcFd));
	if (!SetLimit(RLIMIT_CORE, 0)
		|| !SetLimit(RLIMIT_FSIZE, 0)) {
		return Errno("setrlimit");
	}
	if (options.maxOpenFiles > 0
		&& !SetLimit(RLIMIT_NOFILE, rlim_t(options.maxOpenFiles))) {
		return Errno("RLIMIT_NOFILE");
	}
	if (options.cpuSeconds > 0
		&& !SetLimit(RLIMIT_CPU, rlim_t(options.cpuSeconds))) {
		return Errno("RLIMIT_CPU");
	}
	// RLIMIT_AS is not enforced here, memory is bounded by the guest limits
	// and watched from outside by the process that started the host.
	report.limits = true;
	return report;
}

Result<Report, std::string> Lock(const Options &options, Report report) {
	if (!options.layers) {
		return report;
	}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	char *error = nullptr;
	if (sandbox_init(kProfile, 0, &error) != 0) {
		const auto text = std::string("sandbox_init: ")
			+ (error ? error : "failed");
		if (error) {
			sandbox_free_error(error);
		}
		return text;
	}
#pragma clang diagnostic pop
	// Look at the result instead of trusting the call.
	if (open("/etc/passwd", O_RDONLY) >= 0) {
		return std::string("the sandbox does not hold");
	}
	report.filesystem = true;
	report.processes = true;
	return report;
}

Result<Report, std::string> Apply(const Options &options) {
	auto restricted = Restrict(options);
	if (!restricted.ok()) {
		return restricted.error();
	}
	return Lock(options, restricted.value());
}

} // namespace fgpl::sandbox
