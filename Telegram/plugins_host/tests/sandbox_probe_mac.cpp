#include "fgpl/sandbox.h"

#include <bootstrap.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace {

constexpr auto kIpcFd = 3;

// 0 is "the operation worked", anything else is the errno that stopped it.
int Outcome(bool succeeded) {
	if (succeeded) {
		return 0;
	}
	return errno ? errno : 255;
}

int OpenPasswd() {
	return Outcome(open("/etc/passwd", O_RDONLY) >= 0);
}

int OpenDirRoot() {
	return Outcome(opendir("/") != nullptr);
}

int StatRoot() {
	struct stat info;
	return Outcome(stat("/", &info) == 0);
}

int MakeDirectory() {
	const auto made = mkdir("/tmp/fgpl-probe-dir", 0700) == 0;
	if (made) {
		rmdir("/tmp/fgpl-probe-dir");
	}
	return Outcome(made);
}

int CreateFile() {
	const auto fd = open("/tmp/fgpl-probe-file", O_CREAT | O_WRONLY, 0600);
	if (fd >= 0) {
		close(fd);
		unlink("/tmp/fgpl-probe-file");
	}
	return Outcome(fd >= 0);
}

int SocketInet() {
	return Outcome(socket(AF_INET, SOCK_STREAM, 0) >= 0);
}

int SocketUnix() {
	return Outcome(socket(AF_UNIX, SOCK_STREAM, 0) >= 0);
}

int ConnectUnix() {
	const auto fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return Outcome(false);
	}
	auto address = sockaddr_un();
	address.sun_family = AF_UNIX;
	std::strncpy(address.sun_path, "/var/run/syslog", sizeof(address.sun_path) - 1);
	return Outcome(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
}

int KillParent() {
	return Outcome(kill(getppid(), 0) == 0);
}

int Fork() {
	const auto pid = fork();
	if (pid == 0) {
		_exit(0);
	}
	if (pid > 0) {
		waitpid(pid, nullptr, 0);
	}
	return Outcome(pid > 0);
}

int Execve() {
	char *const arguments[] = { const_cast<char*>("true"), nullptr };
	char *const environment[] = { nullptr };
	execve("/usr/bin/true", arguments, environment);
	return Outcome(false);
}

int Spawn() {
	char *const arguments[] = { const_cast<char*>("true"), nullptr };
	char *const environment[] = { nullptr };
	auto pid = pid_t();
	const auto result = posix_spawn(
		&pid,
		"/usr/bin/true",
		nullptr,
		nullptr,
		arguments,
		environment);
	if (result == 0) {
		waitpid(pid, nullptr, 0);
		return 0;
	}
	errno = result;
	return Outcome(false);
}

int MachLookup() {
	auto port = mach_port_t();
	return Outcome(bootstrap_look_up(
		bootstrap_port,
		"com.apple.system.notification_center",
		&port) == KERN_SUCCESS);
}

int TaskForParent() {
	auto task = mach_port_t();
	return Outcome(task_for_pid(mach_task_self(), getppid(), &task) == KERN_SUCCESS);
}

int SysctlHostname() {
	char name[256];
	auto size = sizeof(name);
	return Outcome(sysctlbyname("kern.hostname", name, &size, nullptr, 0) == 0);
}

int SharedMemory() {
	const auto fd = shm_open("/fgpl-probe-shm", O_CREAT | O_RDWR, 0600);
	if (fd >= 0) {
		close(fd);
		shm_unlink("/fgpl-probe-shm");
	}
	return Outcome(fd >= 0);
}

int WriteIpc() {
	return Outcome(write(kIpcFd, "x", 1) == 1);
}

int ReadIpc() {
	char byte = 0;
	return Outcome(read(kIpcFd, &byte, 1) == 1);
}

int Malloc() {
	const auto block = static_cast<char*>(std::malloc(size_t(8) << 20));
	if (!block) {
		return Outcome(false);
	}
	std::memset(block, 1, size_t(8) << 20);
	std::free(block);
	return 0;
}

int MapAnonymous() {
	const auto block = mmap(
		nullptr,
		1 << 20,
		PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANON,
		-1,
		0);
	if (block == MAP_FAILED) {
		return Outcome(false);
	}
	munmap(block, 1 << 20);
	return 0;
}

void *ThreadBody(void *) {
	return nullptr;
}

int Thread() {
	auto thread = pthread_t();
	if (pthread_create(&thread, nullptr, ThreadBody, nullptr) != 0) {
		return Outcome(false);
	}
	pthread_join(thread, nullptr);
	return 0;
}

int Clock() {
	auto now = timespec();
	return Outcome(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
}

int Sleep() {
	const auto pause = timespec{ .tv_sec = 0, .tv_nsec = 1000000 };
	nanosleep(&pause, nullptr);
	return 0;
}

int SignalSelf() {
	return Outcome(kill(getpid(), 0) == 0);
}

int SysctlPageSize() {
	auto value = int();
	auto size = sizeof(value);
	return Outcome(sysctlbyname("hw.pagesize", &value, &size, nullptr, 0) == 0);
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 3) {
		return 120;
	}
	const auto names = std::map<std::string, int(*)()>{
		{ "open_passwd", OpenPasswd },
		{ "opendir_root", OpenDirRoot },
		{ "stat_root", StatRoot },
		{ "mkdir", MakeDirectory },
		{ "create_file", CreateFile },
		{ "socket_inet", SocketInet },
		{ "socket_unix", SocketUnix },
		{ "connect_unix", ConnectUnix },
		{ "kill_parent", KillParent },
		{ "fork", Fork },
		{ "execve", Execve },
		{ "spawn", Spawn },
		{ "mach_lookup", MachLookup },
		{ "task_for_parent", TaskForParent },
		{ "sysctl_hostname", SysctlHostname },
		{ "shared_memory", SharedMemory },
		{ "write_ipc", WriteIpc },
		{ "read_ipc", ReadIpc },
		{ "malloc", Malloc },
		{ "map_anonymous", MapAnonymous },
		{ "thread", Thread },
		{ "clock", Clock },
		{ "sleep", Sleep },
		{ "signal_self", SignalSelf },
		{ "sysctl_pagesize", SysctlPageSize },
	};
	const auto probe = names.find(argv[2]);
	if (probe == names.end()) {
		return 121;
	}
	auto options = fgpl::sandbox::Options();
	options.ipcFd = kIpcFd;
	options.layers = (std::string(argv[1]) == "all")
		? fgpl::sandbox::kLayersAll
		: 0;
	const auto applied = fgpl::sandbox::Apply(options);
	if (!applied.ok()) {
		std::fprintf(stderr, "sandbox: %s\n", applied.error().c_str());
		return 122;
	}
	return probe->second();
}
