#include "check.h"
#include "sandbox_linux.h"

#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Outcome {
	bool exited = false;
	int code = 0;
	int signal = 0;
};

[[nodiscard]] Outcome Run(const std::string &layers, const std::string &probe) {
	int pair[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
		return {};
	}
	if (write(pair[0], "x", 1) != 1) {
		return {};
	}
	const auto pid = fork();
	if (pid == 0) {
		if (dup2(pair[1], 3) < 0) {
			_exit(123);
		}
		execl(
			FGPL_SANDBOX_PROBE,
			FGPL_SANDBOX_PROBE,
			layers.c_str(),
			probe.c_str(),
			nullptr);
		_exit(124);
	}
	close(pair[1]);
	auto status = 0;
	waitpid(pid, &status, 0);
	close(pair[0]);
	auto result = Outcome();
	if (WIFEXITED(status)) {
		result.exited = true;
		result.code = WEXITSTATUS(status);
	} else if (WIFSIGNALED(status)) {
		result.signal = WTERMSIG(status);
	}
	return result;
}

void CheckKilled(const std::string &probe) {
	const auto outcome = Run("all", probe);
	if (outcome.signal != SIGSYS) {
		std::fprintf(
			stderr,
			"%s: expected SIGSYS, got exited=%d code=%d signal=%d\n",
			probe.c_str(),
			outcome.exited,
			outcome.code,
			outcome.signal);
		CHECK(false);
	}
}

void CheckAllowed(const std::string &probe) {
	const auto outcome = Run("all", probe);
	if (!outcome.exited || outcome.code != 0) {
		std::fprintf(
			stderr,
			"%s: expected success, got exited=%d code=%d signal=%d\n",
			probe.c_str(),
			outcome.exited,
			outcome.code,
			outcome.signal);
		CHECK(false);
	}
}

void TestSeccompKillsEverythingElse() {
	const auto blocked = std::vector<std::string>{
		"open_passwd", "open_proc_mem", "open_parent_mem", "opendir_root",
		"stat_root", "mkdir", "create_file", "unlink", "connect_abstract",
		"connect_tcp", "kill_parent", "socket_inet", "socket_unix",
		"socket_netlink", "ptrace", "process_vm_read", "fork", "thread",
		"execve", "mount", "unshare_user", "setns", "bpf", "io_uring",
		"perf_event", "userfaultfd", "keyctl", "memfd", "personality",
		"chroot", "install_filter", "setuid", "openat2", "dup", "pipe",
		"fcntl", "ioctl", "eventfd", "inotify", "readlink", "getcwd",
		"uname", "sysinfo", "set_rlimit", "write_stdout", "read_stdin",
		"sendmsg", "clock_settime", "futex_requeue",
	};
	for (const auto &probe : blocked) {
		CheckKilled(probe);
	}
}

void TestWhatTheHostNeedsStillWorks() {
	const auto allowed = std::vector<std::string>{
		"write_ipc", "read_ipc", "write_stderr", "map_anonymous",
		"jit_like_code", "futex", "clock", "random", "signals",
		"read_limits", "malloc", "sleep", "yield",
	};
	for (const auto &probe : allowed) {
		CheckAllowed(probe);
	}
}

struct LandlockCase {
	const char *probe;
	int baseline;
	int minimumAbi;
	int blockedErrno;
};

void TestLandlockAloneBlocksFilesAndSignals() {
	const auto abi = fgpl::sandbox::LandlockAbi();
	if (abi == 0) {
		std::fprintf(stderr, "Landlock is not available, skipping\n");
		return;
	}
	auto listener = socket(AF_UNIX, SOCK_STREAM, 0);
	auto address = sockaddr_un();
	address.sun_family = AF_UNIX;
	const auto name = std::string("fgpl-sandbox-test-listener");
	std::memcpy(address.sun_path + 1, name.data(), name.size());
	const auto length = socklen_t(
		offsetof(sockaddr_un, sun_path) + 1 + name.size());
	CHECK(bind(listener, reinterpret_cast<sockaddr *>(&address), length) == 0);
	CHECK(listen(listener, 8) == 0);

	const auto cases = std::vector<LandlockCase>{
		{ "open_passwd", 0, 1, EACCES },
		{ "opendir_root", 0, 1, EACCES },
		{ "mkdir", 0, 1, EACCES },
		{ "create_file", 0, 1, EACCES },
		{ "connect_tcp", ECONNREFUSED, 4, EACCES },
		{ "connect_abstract", 0, 6, EPERM },
		{ "kill_parent", 0, 6, EPERM },
	};
	for (const auto &item : cases) {
		const auto baseline = Run("none", item.probe);
		if (!baseline.exited || baseline.code != item.baseline) {
			std::fprintf(
				stderr,
				"%s: without the sandbox expected %d, got exited=%d code=%d signal=%d\n",
				item.probe,
				item.baseline,
				baseline.exited,
				baseline.code,
				baseline.signal);
			CHECK(false);
		}
		if (abi < item.minimumAbi) {
			continue;
		}
		const auto outcome = Run("landlock", item.probe);
		if (!outcome.exited || outcome.code != item.blockedErrno) {
			std::fprintf(
				stderr,
				"%s: with Landlock expected %d, got exited=%d code=%d signal=%d\n",
				item.probe,
				item.blockedErrno,
				outcome.exited,
				outcome.code,
				outcome.signal);
			CHECK(false);
		}
	}
	close(listener);
}

void TestReport() {
	CHECK(fgpl::sandbox::LandlockAbi() >= 0);
}

} // namespace

int main() {
	TestReport();
	TestLandlockAloneBlocksFilesAndSignals();
	TestSeccompKillsEverythingElse();
	TestWhatTheHostNeedsStillWorks();
	return Failures() ? 1 : 0;
}
