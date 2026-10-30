#include "fgpl/os.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace fgpl::os {
namespace {

constexpr auto kHostFd = 3;
constexpr auto kExecutableFd = 4;

void ConvertStatus(int status, ExitInfo &info) {
	info = ExitInfo();
	if (WIFSIGNALED(status)) {
		info.signaled = true;
		info.signal = WTERMSIG(status);
	} else if (WIFEXITED(status)) {
		info.code = WEXITSTATUS(status);
	}
}

#ifdef __APPLE__

// There is no fexecve on macOS, the sealed copy is started by its path.
[[nodiscard]] std::string ExecutablePath(Handle executable) {
	char path[1024] = {};
	if (fcntl(int(executable), F_GETPATH, path) != 0) {
		return {};
	}
	return path;
}

#endif // __APPLE__

} // namespace

struct Child::Native {
	pid_t pid = -1;
};

Child::Child(std::unique_ptr<Native> native)
: _native(std::move(native)) {
}

Child::~Child() = default;

Result<std::unique_ptr<Child>, std::string> Child::Spawn(
		const SpawnSpec &spec,
		Handle &channel) {
	int pair[2];
#ifdef __APPLE__
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
		return std::string(std::strerror(errno));
	}
	fcntl(pair[0], F_SETFD, FD_CLOEXEC);
	fcntl(pair[1], F_SETFD, FD_CLOEXEC);
#else // __APPLE__
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
		return std::string(std::strerror(errno));
	}
#endif // __APPLE__
	signal(SIGPIPE, SIG_IGN);
	auto actions = posix_spawn_file_actions_t();
	posix_spawn_file_actions_init(&actions);
	auto attributes = posix_spawnattr_t();
	posix_spawnattr_init(&attributes);
#ifdef __APPLE__
	posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif // __APPLE__
	posix_spawn_file_actions_adddup2(&actions, pair[1], kHostFd);
	for (const auto fd : { 0, 1, 2 }) {
		posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", O_RDWR, 0);
	}
	const auto cleanup = [&] {
		posix_spawn_file_actions_destroy(&actions);
		posix_spawnattr_destroy(&attributes);
	};
	auto program = spec.path;
	auto executable = -1;
	if (spec.executable >= 0) {
#ifdef __APPLE__
		program = ExecutablePath(spec.executable);
		if (program.empty()) {
			cleanup();
			close(pair[0]);
			close(pair[1]);
			return std::string("cannot find the sealed host");
		}
#else // __APPLE__
		executable = fcntl(int(spec.executable), F_DUPFD_CLOEXEC, 10);
		if (executable < 0) {
			const auto error = std::string(std::strerror(errno));
			cleanup();
			close(pair[0]);
			close(pair[1]);
			return error;
		}
		posix_spawn_file_actions_adddup2(&actions, executable, kExecutableFd);
		program = "/proc/self/fd/" + std::to_string(kExecutableFd);
#endif // __APPLE__
	}
	auto arguments = std::vector<std::string>{
		spec.path,
		"--ipc-fd=" + std::to_string(kHostFd),
	};
	for (const auto &argument : spec.arguments) {
		arguments.push_back(argument);
	}
	auto argv = std::vector<char *>();
	for (auto &argument : arguments) {
		argv.push_back(argument.data());
	}
	argv.push_back(nullptr);
	char *environment[] = { nullptr };
	auto pid = pid_t();
	const auto spawned = posix_spawn(
		&pid,
		program.c_str(),
		&actions,
		&attributes,
		argv.data(),
		environment);
	cleanup();
	if (executable >= 0) {
		close(executable);
	}
	close(pair[1]);
	if (spec.sendTimeoutMs > 0) {
		const auto timeout = timeval{
			.tv_sec = spec.sendTimeoutMs / 1000,
			.tv_usec = (spec.sendTimeoutMs % 1000) * 1000,
		};
		setsockopt(pair[0], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	}
	if (spawned != 0) {
		close(pair[0]);
		return std::string(std::strerror(spawned));
	}
	channel = pair[0];
	auto native = std::make_unique<Native>();
	native->pid = pid;
	return std::unique_ptr<Child>(new Child(std::move(native)));
}

int Child::id() const {
	return int(_native->pid);
}

void Child::terminate() {
	::kill(_native->pid, SIGKILL);
}

bool Child::tryWait(ExitInfo &info) {
	auto status = 0;
	while (true) {
		const auto waited = waitpid(_native->pid, &status, WNOHANG);
		if (waited < 0 && errno == EINTR) {
			continue;
		} else if (waited != _native->pid) {
			return false;
		}
		ConvertStatus(status, info);
		return true;
	}
}

void Child::wait(ExitInfo &info) {
	auto status = 0;
	while (true) {
		const auto waited = waitpid(_native->pid, &status, 0);
		if (waited < 0 && errno == EINTR) {
			continue;
		} else if (waited == _native->pid) {
			ConvertStatus(status, info);
		}
		return;
	}
}

int SandboxViolationSignal() {
#ifdef __linux__
	return SIGSYS;
#else // __linux__
	return 0;
#endif // __linux__
}

} // namespace fgpl::os
