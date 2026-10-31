#include "check.h"

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace {

// The probe's own codes for "bad arguments" and "the sandbox did not apply".
constexpr auto kFirstProbeFailure = 120;

struct Outcome {
	bool exited = false;
	int code = -1;
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

void Report(const char *what, const std::string &probe, const Outcome &outcome) {
	std::fprintf(
		stderr,
		"%s: %s: exited=%d code=%d signal=%d\n",
		probe.c_str(),
		what,
		outcome.exited,
		outcome.code,
		outcome.signal);
}

// Refused means the operation failed or the process was killed, not that the probe broke.
void CheckRefused(const std::string &probe) {
	const auto outcome = Run("all", probe);
	const auto refused = outcome.signal != 0
		|| (outcome.exited
			&& outcome.code != 0
			&& outcome.code < kFirstProbeFailure);
	if (!refused) {
		Report("was not refused", probe, outcome);
		CHECK(false);
	}
}

void CheckAllowed(const std::string &layers, const std::string &probe) {
	const auto outcome = Run(layers, probe);
	if (!outcome.exited || outcome.code != 0) {
		Report("was expected to work", probe, outcome);
		CHECK(false);
	}
}

// WHY: without the sandbox the same operations work, so a refusal is the sandbox and not a broken probe.
void TestProbesWorkWithoutTheSandbox() {
	const auto controls = std::vector<std::string>{
		"open_passwd", "opendir_root", "stat_root", "mkdir", "create_file",
		"socket_inet", "socket_unix", "kill_parent", "fork", "spawn",
		"sysctl_hostname", "shared_memory", "write_ipc", "read_ipc",
	};
	for (const auto &probe : controls) {
		CheckAllowed("none", probe);
	}
}

void TestSandboxRefusesTheRest() {
	const auto refused = std::vector<std::string>{
		"open_passwd", "opendir_root", "stat_root", "mkdir", "create_file",
		"socket_inet", "socket_unix", "connect_unix", "kill_parent", "fork",
		"execve", "spawn", "mach_lookup", "task_for_parent",
		"sysctl_hostname", "shared_memory",
	};
	for (const auto &probe : refused) {
		CheckRefused(probe);
	}
}

void TestWhatTheHostNeedsStillWorks() {
	const auto allowed = std::vector<std::string>{
		"write_ipc", "read_ipc", "malloc", "map_anonymous", "thread", "clock",
		"sleep", "signal_self", "sysctl_pagesize",
	};
	for (const auto &probe : allowed) {
		CheckAllowed("all", probe);
	}
}

} // namespace

int main() {
	TestProbesWorkWithoutTheSandbox();
	TestSandboxRefusesTheRest();
	TestWhatTheHostNeedsStillWorks();
	return Failures() ? 1 : 0;
}
