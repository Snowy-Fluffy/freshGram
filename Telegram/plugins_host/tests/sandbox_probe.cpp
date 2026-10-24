#include "fgpl/sandbox_linux.h"

#include <fcntl.h>
#include <linux/futex.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/personality.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <sched.h>
#include <time.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>

namespace {

constexpr auto kIpcFd = 3;
constexpr auto kAbstractName = "fgpl-sandbox-test-listener";

int Outcome(bool succeeded) {
	if (succeeded) {
		return 0;
	}
	return errno ? errno : 255;
}

int OpenPasswd() {
	const auto fd = open("/etc/passwd", O_RDONLY);
	return Outcome(fd >= 0);
}

int OpenProcMem() {
	const auto fd = open("/proc/self/mem", O_RDWR);
	return Outcome(fd >= 0);
}

int OpenParentMem() {
	char path[64];
	std::snprintf(path, sizeof(path), "/proc/%d/mem", int(getppid()));
	const auto fd = open(path, O_RDONLY);
	return Outcome(fd >= 0);
}

int OpenDirRoot() {
	const auto dir = opendir("/");
	return Outcome(dir != nullptr);
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

int Unlink() {
	return Outcome(unlink("/etc/fgpl-probe-missing") == 0);
}

int ConnectAbstract() {
	const auto fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return Outcome(false);
	}
	auto address = sockaddr_un();
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path + 1, kAbstractName, std::strlen(kAbstractName));
	const auto length = socklen_t(
		offsetof(sockaddr_un, sun_path) + 1 + std::strlen(kAbstractName));
	return Outcome(connect(fd, reinterpret_cast<sockaddr *>(&address), length) == 0);
}

int ConnectTcp() {
	const auto fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return Outcome(false);
	}
	auto address = sockaddr_in();
	address.sin_family = AF_INET;
	address.sin_port = htons(1);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	return Outcome(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
}

int KillParent() {
	return Outcome(kill(getppid(), 0) == 0);
}

int SocketInet() {
	return Outcome(socket(AF_INET, SOCK_DGRAM, 0) >= 0);
}

int SocketUnix() {
	return Outcome(socket(AF_UNIX, SOCK_STREAM, 0) >= 0);
}

int SocketNetlink() {
	return Outcome(socket(AF_NETLINK, SOCK_RAW, 0) >= 0);
}

int Ptrace() {
	return Outcome(ptrace(PTRACE_PEEKTEXT, getppid(), nullptr, nullptr) != -1);
}

int ProcessVmRead() {
	char buffer[8];
	auto local = iovec{ buffer, sizeof(buffer) };
	auto remote = iovec{ reinterpret_cast<void *>(&buffer), sizeof(buffer) };
	return Outcome(process_vm_readv(getppid(), &local, 1, &remote, 1, 0) > 0);
}

int Fork() {
	const auto pid = syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0);
	if (pid == 0) {
		_exit(0);
	}
	return Outcome(pid > 0);
}

int Thread() {
	try {
		std::thread([] {}).join();
		return 0;
	} catch (...) {
		return 255;
	}
}

int Execve() {
	char *const arguments[] = { const_cast<char *>("true"), nullptr };
	execv("/bin/true", arguments);
	return Outcome(false);
}

int Mount() {
	return Outcome(mount("none", "/tmp", "tmpfs", 0, nullptr) == 0);
}

int UnshareUser() {
	return Outcome(unshare(CLONE_NEWUSER) == 0);
}

int Setns() {
	return Outcome(syscall(SYS_setns, -1, 0) == 0);
}

int Bpf() {
	return Outcome(syscall(SYS_bpf, 0, nullptr, 0) >= 0);
}

int IoUring() {
	return Outcome(syscall(SYS_io_uring_setup, 8, nullptr) >= 0);
}

int PerfEvent() {
	return Outcome(syscall(SYS_perf_event_open, nullptr, 0, -1, -1, 0) >= 0);
}

int UserFaultFd() {
	return Outcome(syscall(SYS_userfaultfd, 0) >= 0);
}

int Keyctl() {
	return Outcome(syscall(SYS_keyctl, 0, 0, 0, 0, 0) >= 0);
}

int MemFd() {
	return Outcome(syscall(SYS_memfd_create, "x", 0) >= 0);
}

int Personality() {
	return Outcome(personality(0xffffffff) != -1);
}

int Chroot() {
	return Outcome(chroot("/tmp") == 0);
}

int InstallFilter() {
	return Outcome(prctl(PR_SET_SECCOMP, 1) == 0);
}

int SetUid() {
	return Outcome(setuid(0) == 0);
}

int Openat2() {
	return Outcome(syscall(SYS_openat2, -100, "/etc/passwd", nullptr, 0) >= 0);
}

int Dup() {
	return Outcome(dup(kIpcFd) >= 0);
}

int Pipe() {
	int fds[2];
	return Outcome(pipe(fds) == 0);
}

int Fcntl() {
	return Outcome(fcntl(kIpcFd, F_GETFL) >= 0);
}

int Ioctl() {
	return Outcome(ioctl(0, 0x5401, nullptr) >= 0);
}

int Eventfd() {
	return Outcome(syscall(SYS_eventfd2, 0, 0) >= 0);
}

int InotifyInit() {
	return Outcome(syscall(SYS_inotify_init1, 0) >= 0);
}

int Readlink() {
	char buffer[64];
	return Outcome(readlink("/proc/self/exe", buffer, sizeof(buffer)) >= 0);
}

int Getcwd() {
	char buffer[64];
	return Outcome(getcwd(buffer, sizeof(buffer)) != nullptr);
}

int Uname() {
	utsname info;
	return Outcome(uname(&info) == 0);
}

int Sysinfo() {
	return Outcome(syscall(SYS_sysinfo, nullptr) >= 0);
}

int SetRlimit() {
	const auto limit = rlimit{ 1 << 20, 1 << 20 };
	return Outcome(setrlimit(RLIMIT_NOFILE, &limit) == 0);
}

int WriteStdout() {
	return Outcome(write(1, "x", 1) == 1);
}

int ReadStdin() {
	char buffer[1];
	return Outcome(read(0, buffer, 1) >= 0);
}

int Sendmsg() {
	msghdr header{};
	return Outcome(sendmsg(kIpcFd, &header, 0) >= 0);
}

int ClockSettime() {
	auto now = timespec{ 0, 0 };
	return Outcome(clock_settime(CLOCK_REALTIME, &now) == 0);
}

int WriteIpc() {
	return Outcome(write(kIpcFd, "x", 1) == 1);
}

int ReadIpc() {
	char buffer[1];
	return Outcome(read(kIpcFd, buffer, 1) == 1);
}

int WriteStderr() {
	return Outcome(write(2, "", 0) == 0);
}

int MapAnonymous() {
	const auto memory = mmap(
		nullptr,
		1 << 20,
		PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS,
		-1,
		0);
	if (memory == MAP_FAILED) {
		return Outcome(false);
	}
	static_cast<char *>(memory)[4096] = 1;
	const auto advised = madvise(memory, 1 << 20, MADV_DONTNEED) == 0;
	return Outcome(munmap(memory, 1 << 20) == 0 && advised);
}

int JitLikeCode() {
	const auto memory = mmap(
		nullptr,
		4096,
		PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS,
		-1,
		0);
	if (memory == MAP_FAILED) {
		return Outcome(false);
	}
#if defined(__x86_64__)
	const unsigned char code[] = { 0xB8, 0x2A, 0, 0, 0, 0xC3 };
#else
	const unsigned char code[] = {
		0x40, 0x05, 0x80, 0x52,
		0xC0, 0x03, 0x5F, 0xD6,
	};
#endif
	std::memcpy(memory, code, sizeof(code));
	if (mprotect(memory, 4096, PROT_READ | PROT_EXEC) != 0) {
		return Outcome(false);
	}
	__builtin___clear_cache(
		static_cast<char *>(memory),
		static_cast<char *>(memory) + sizeof(code));
	const auto value = reinterpret_cast<int (*)()>(memory)();
	return Outcome(value == 42);
}

int Futex() {
	static std::atomic<int> word{ 0 };
	return Outcome(syscall(
		SYS_futex,
		&word,
		FUTEX_WAKE_PRIVATE,
		1,
		nullptr,
		nullptr,
		0) >= 0);
}

int FutexRequeue() {
	static std::atomic<int> word{ 0 };
	return Outcome(syscall(
		SYS_futex,
		&word,
		FUTEX_CMP_REQUEUE_PRIVATE,
		1,
		nullptr,
		nullptr,
		0) >= 0);
}

int Clock() {
	auto now = timespec();
	return Outcome(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
}

int Random() {
	char buffer[16];
	return Outcome(getrandom(buffer, sizeof(buffer), 0) == 16);
}

int Signals() {
	struct sigaction action{};
	action.sa_handler = SIG_IGN;
	auto stack = stack_t();
	const auto altered = sigaltstack(nullptr, &stack) == 0;
	auto set = sigset_t();
	sigemptyset(&set);
	return Outcome(altered
		&& sigaction(SIGUSR1, &action, nullptr) == 0
		&& pthread_sigmask(SIG_BLOCK, &set, nullptr) == 0);
}

int ReadLimits() {
	auto limit = rlimit();
	return Outcome(getrlimit(RLIMIT_NOFILE, &limit) == 0);
}

int Malloc() {
	const auto memory = std::malloc(4 << 20);
	if (!memory) {
		return Outcome(false);
	}
	std::memset(memory, 1, 4 << 20);
	std::free(memory);
	return 0;
}

int Sleep() {
	const auto pause = timespec{ 0, 1000000 };
	return Outcome(nanosleep(&pause, nullptr) == 0);
}

int Yield() {
	return Outcome(sched_yield() == 0);
}

int Speculation() {
	return Outcome(fgpl::sandbox::SpeculationRestricted());
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 3) {
		return 120;
	}
	const auto names = std::map<std::string, int (*)()>{
		{ "open_passwd", OpenPasswd },
		{ "open_proc_mem", OpenProcMem },
		{ "open_parent_mem", OpenParentMem },
		{ "opendir_root", OpenDirRoot },
		{ "stat_root", StatRoot },
		{ "mkdir", MakeDirectory },
		{ "create_file", CreateFile },
		{ "unlink", Unlink },
		{ "connect_abstract", ConnectAbstract },
		{ "connect_tcp", ConnectTcp },
		{ "kill_parent", KillParent },
		{ "socket_inet", SocketInet },
		{ "socket_unix", SocketUnix },
		{ "socket_netlink", SocketNetlink },
		{ "ptrace", Ptrace },
		{ "process_vm_read", ProcessVmRead },
		{ "fork", Fork },
		{ "thread", Thread },
		{ "execve", Execve },
		{ "mount", Mount },
		{ "unshare_user", UnshareUser },
		{ "setns", Setns },
		{ "bpf", Bpf },
		{ "io_uring", IoUring },
		{ "perf_event", PerfEvent },
		{ "userfaultfd", UserFaultFd },
		{ "keyctl", Keyctl },
		{ "memfd", MemFd },
		{ "personality", Personality },
		{ "chroot", Chroot },
		{ "install_filter", InstallFilter },
		{ "setuid", SetUid },
		{ "openat2", Openat2 },
		{ "dup", Dup },
		{ "pipe", Pipe },
		{ "fcntl", Fcntl },
		{ "ioctl", Ioctl },
		{ "eventfd", Eventfd },
		{ "inotify", InotifyInit },
		{ "readlink", Readlink },
		{ "getcwd", Getcwd },
		{ "uname", Uname },
		{ "sysinfo", Sysinfo },
		{ "set_rlimit", SetRlimit },
		{ "write_stdout", WriteStdout },
		{ "read_stdin", ReadStdin },
		{ "sendmsg", Sendmsg },
		{ "clock_settime", ClockSettime },
		{ "futex_requeue", FutexRequeue },
		{ "write_ipc", WriteIpc },
		{ "read_ipc", ReadIpc },
		{ "write_stderr", WriteStderr },
		{ "map_anonymous", MapAnonymous },
		{ "jit_like_code", JitLikeCode },
		{ "futex", Futex },
		{ "clock", Clock },
		{ "random", Random },
		{ "signals", Signals },
		{ "read_limits", ReadLimits },
		{ "malloc", Malloc },
		{ "sleep", Sleep },
		{ "yield", Yield },
		{ "speculation", Speculation },
	};
	const auto probe = names.find(argv[2]);
	if (probe == names.end()) {
		return 121;
	}
	const auto layers = std::string(argv[1]);
	auto options = fgpl::sandbox::Options();
	options.ipcFd = kIpcFd;
	options.layers = (layers == "all")
		? fgpl::sandbox::kLayersAll
		: (layers == "landlock")
		? (fgpl::sandbox::kLayerRlimits | fgpl::sandbox::kLayerLandlock)
		: 0;
	const auto applied = fgpl::sandbox::Apply(options);
	if (!applied.ok()) {
		std::fprintf(stderr, "sandbox: %s\n", applied.error().c_str());
		return 122;
	}
	return probe->second();
}
