#include "fgpl/sandbox_linux.h"

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <bit>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <vector>

#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#define SYS_landlock_restrict_self 446
#endif

#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif

#ifndef PR_SPEC_INDIRECT_BRANCH
#define PR_SPEC_INDIRECT_BRANCH 1
#endif

namespace fgpl::sandbox {
namespace {

static_assert(std::endian::native == std::endian::little);

constexpr auto kFsMaskV1 = uint64_t(0x1FFF);
constexpr auto kFsRefer = uint64_t(1) << 13;
constexpr auto kFsTruncate = uint64_t(1) << 14;
constexpr auto kFsIoctlDev = uint64_t(1) << 15;
constexpr auto kNetMask = uint64_t(3);
constexpr auto kScopeMask = uint64_t(3);
constexpr auto kCreateRulesetVersion = uint32_t(1);

struct RulesetAttr {
	uint64_t handledFs = 0;
	uint64_t handledNet = 0;
	uint64_t scoped = 0;
};

#if defined(__x86_64__)
constexpr auto kAuditArch = uint32_t(AUDIT_ARCH_X86_64);
#elif defined(__aarch64__)
constexpr auto kAuditArch = uint32_t(AUDIT_ARCH_AARCH64);
#else
#error "the seccomp filter is written for x86_64 and aarch64"
#endif

constexpr auto kMembarrierSyncCore = uint32_t(1) << 5;
constexpr auto kMembarrierRegisterSyncCore = uint32_t(1) << 6;

constexpr int kSpeculation[] = { PR_SPEC_STORE_BYPASS, PR_SPEC_INDIRECT_BRANCH };

void RestrictSpeculation() {
	for (const auto which : kSpeculation) {
		prctl(PR_SET_SPECULATION_CTRL, which, PR_SPEC_FORCE_DISABLE, 0, 0);
	}
}

[[nodiscard]] std::string Errno(const char *what) {
	return std::string(what) + ": " + std::strerror(errno);
}

class Program {
public:
	using Label = size_t;

	[[nodiscard]] Label label() {
		_positions.push_back(kUnbound);
		return _positions.size() - 1;
	}

	void bind(Label target) {
		_positions[target] = _code.size();
	}

	void load(uint32_t offset) {
		_code.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offset));
	}

	void mask(uint32_t value) {
		_code.push_back(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, value));
	}

	void result(uint32_t action) {
		_code.push_back(BPF_STMT(BPF_RET | BPF_K, action));
	}

	void equal(uint32_t value, Label ifTrue, Label ifFalse) {
		_pending.push_back({ _code.size(), ifTrue, ifFalse });
		_code.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, value, 0, 0));
	}

	void atLeast(uint32_t value, Label ifTrue, Label ifFalse) {
		_pending.push_back({ _code.size(), ifTrue, ifFalse });
		_code.push_back(BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, value, 0, 0));
	}

	[[nodiscard]] bool finish(std::vector<sock_filter> &out) {
		for (const auto &jump : _pending) {
			const auto yes = _positions[jump.yes] - (jump.index + 1);
			const auto no = _positions[jump.no] - (jump.index + 1);
			if (yes > 255 || no > 255) {
				return false;
			}
			_code[jump.index].jt = uint8_t(yes);
			_code[jump.index].jf = uint8_t(no);
		}
		out = std::move(_code);
		return true;
	}

private:
	struct Pending {
		size_t index = 0;
		Label yes = 0;
		Label no = 0;
	};

	static constexpr auto kUnbound = size_t(-1);

	std::vector<sock_filter> _code;
	std::vector<size_t> _positions;
	std::vector<Pending> _pending;

};

[[nodiscard]] std::vector<uint32_t> PlainSyscalls() {
	return {
		uint32_t(SYS_mmap),
		uint32_t(SYS_munmap),
		uint32_t(SYS_mprotect),
		uint32_t(SYS_madvise),
		uint32_t(SYS_mremap),
		uint32_t(SYS_brk),
		uint32_t(SYS_close),
		uint32_t(SYS_rt_sigreturn),
		uint32_t(SYS_rt_sigaction),
		uint32_t(SYS_rt_sigprocmask),
		uint32_t(SYS_sigaltstack),
		uint32_t(SYS_sched_yield),
		uint32_t(SYS_clock_gettime),
		uint32_t(SYS_clock_nanosleep),
		uint32_t(SYS_getrandom),
		uint32_t(SYS_exit),
		uint32_t(SYS_exit_group),
		uint32_t(SYS_gettid),
		uint32_t(SYS_getpid),
#ifdef SYS_nanosleep
		uint32_t(SYS_nanosleep),
#endif
#ifdef SYS_gettimeofday
		uint32_t(SYS_gettimeofday),
#endif
#ifdef SYS_clock_getres
		uint32_t(SYS_clock_getres),
#endif
	};
}

[[nodiscard]] Result<std::vector<sock_filter>, std::string> BuildFilter(
		const Options &options) {
	constexpr auto kArchOffset = uint32_t(offsetof(seccomp_data, arch));
	constexpr auto kNrOffset = uint32_t(offsetof(seccomp_data, nr));
	const auto argLow = [](int index) {
		return uint32_t(offsetof(seccomp_data, args) + 8 * index);
	};
	const auto argHigh = [&](int index) {
		return argLow(index) + 4;
	};

	auto program = Program();
	const auto allow = program.label();
	const auto kill = program.label();
	const auto start = program.label();

	program.load(kArchOffset);
	program.equal(kAuditArch, start, kill);
	program.bind(start);
	program.load(kNrOffset);
	{
		const auto native = program.label();
		program.atLeast(0x40000000, kill, native);
		program.bind(native);
	}
	for (const auto number : PlainSyscalls()) {
		const auto next = program.label();
		program.equal(number, allow, next);
		program.bind(next);
	}

	const auto conditional = [&](uint32_t number, auto &&body) {
		const auto mine = program.label();
		const auto next = program.label();
		program.load(kNrOffset);
		program.equal(number, mine, next);
		program.bind(mine);
		body();
		program.bind(next);
	};
	const auto fdIn = [&](uint32_t number, std::vector<int> fds) {
		conditional(number, [&] {
			program.load(argLow(0));
			for (const auto fd : fds) {
				const auto next = program.label();
				program.equal(uint32_t(fd), allow, next);
				program.bind(next);
			}
			program.result(SECCOMP_RET_KILL_PROCESS);
		});
	};

	auto writable = std::vector<int>{ options.ipcFd };
	if (options.allowStderr) {
		writable.push_back(2);
	}
	fdIn(uint32_t(SYS_read), { options.ipcFd });
	fdIn(uint32_t(SYS_write), writable);
	conditional(uint32_t(SYS_tgkill), [&] {
		program.load(argLow(0));
		program.equal(uint32_t(getpid()), allow, kill);
	});
	conditional(uint32_t(SYS_prlimit64), [&] {
		const auto lowZero = program.label();
		program.load(argLow(2));
		program.equal(0, lowZero, kill);
		program.bind(lowZero);
		program.load(argHigh(2));
		program.equal(0, allow, kill);
	});
#if defined(__aarch64__)
	conditional(uint32_t(SYS_membarrier), [&] {
		program.load(argLow(0));
		for (const auto command : { kMembarrierSyncCore, kMembarrierRegisterSyncCore }) {
			const auto next = program.label();
			program.equal(command, allow, next);
			program.bind(next);
		}
		program.result(SECCOMP_RET_KILL_PROCESS);
	});
#endif
	conditional(uint32_t(SYS_futex), [&] {
		program.load(argLow(1));
		program.mask(0xFFFFFE7F);
		for (const auto operation : { 0u, 1u, 9u, 10u }) {
			const auto next = program.label();
			program.equal(operation, allow, next);
			program.bind(next);
		}
		program.result(SECCOMP_RET_KILL_PROCESS);
	});

	program.bind(kill);
	program.result(SECCOMP_RET_KILL_PROCESS);
	program.bind(allow);
	program.result(SECCOMP_RET_ALLOW);

	auto code = std::vector<sock_filter>();
	if (!program.finish(code)) {
		return std::string("the seccomp program is too long for relative jumps");
	}
	return code;
}

[[nodiscard]] uint64_t FsMaskFor(int abi) {
	auto mask = kFsMaskV1;
	if (abi >= 2) {
		mask |= kFsRefer;
	}
	if (abi >= 3) {
		mask |= kFsTruncate;
	}
	if (abi >= 5) {
		mask |= kFsIoctlDev;
	}
	return mask;
}

[[nodiscard]] bool CloseExtraDescriptors(int keep) {
	if (keep < 3) {
		return false;
	}
#ifdef SYS_close_range
	if (keep > 3 && syscall(SYS_close_range, 3, keep - 1, 0) != 0) {
		return false;
	}
	return syscall(SYS_close_range, keep + 1, ~0u, 0) == 0;
#else
	for (auto fd = 3; fd != 4096; ++fd) {
		if (fd != keep) {
			close(fd);
		}
	}
	return true;
#endif
}

[[nodiscard]] bool Limit(int resource, rlim_t value) {
	const auto limit = rlimit{ value, value };
	return setrlimit(resource, &limit) == 0;
}

} // namespace

bool SpeculationRestricted() {
	for (const auto which : kSpeculation) {
		const auto state = prctl(PR_GET_SPECULATION_CTRL, which, 0, 0, 0);
		if (state < 0) {
			if (errno == ENODEV || errno == EINVAL) {
				continue;
			}
			return false;
		} else if ((state & PR_SPEC_PRCTL) && !(state & PR_SPEC_FORCE_DISABLE)
				&& !(state & PR_SPEC_DISABLE)) {
			return false;
		}
	}
	return true;
}

int LandlockAbi() {
	const auto abi = syscall(
		SYS_landlock_create_ruleset,
		nullptr,
		0,
		kCreateRulesetVersion);
	return (abi > 0) ? int(abi) : 0;
}

Result<Report, std::string> Restrict(const Options &options, Report report) {
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
		return Errno("PR_SET_NO_NEW_PRIVS");
	} else if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
		return Errno("PR_SET_DUMPABLE");
	} else if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0) {
		return Errno("PR_SET_PDEATHSIG");
	} else if (!CloseExtraDescriptors(options.ipcFd)) {
		return Errno("closing inherited descriptors");
	}
	RestrictSpeculation();
	if (!SpeculationRestricted()) {
		return std::string("speculation controls could not be applied");
	}
	if ((options.layers & kLayerRlimits) != 0) {
		if (!Limit(RLIMIT_CORE, 0)
			|| !Limit(RLIMIT_FSIZE, 0)
			|| !Limit(RLIMIT_MEMLOCK, 0)
			|| !Limit(RLIMIT_MSGQUEUE, 0)
			|| !Limit(RLIMIT_NOFILE, rlim_t(options.maxOpenFiles))) {
			return Errno("setrlimit");
		}
		if (options.addressSpaceBytes != 0
			&& !Limit(RLIMIT_AS, rlim_t(options.addressSpaceBytes))) {
			return Errno("setrlimit(RLIMIT_AS)");
		}
		if (options.cpuSeconds != 0
			&& !Limit(RLIMIT_CPU, rlim_t(options.cpuSeconds))) {
			return Errno("setrlimit(RLIMIT_CPU)");
		}
		report.rlimits = true;
	}
	if ((options.layers & kLayerLandlock) != 0) {
		report.landlockAbi = LandlockAbi();
		if (report.landlockAbi == 0) {
			return std::string("Landlock is not available");
		}
		const auto abi = report.landlockAbi;
		auto attr = RulesetAttr{
			.handledFs = FsMaskFor(abi),
			.handledNet = (abi >= 4) ? kNetMask : 0,
			.scoped = (abi >= 6) ? kScopeMask : 0,
		};
		const auto size = (abi >= 6) ? 24 : (abi >= 4) ? 16 : 8;
		const auto ruleset = syscall(
			SYS_landlock_create_ruleset,
			&attr,
			size,
			0);
		if (ruleset < 0) {
			return Errno("landlock_create_ruleset");
		}
		const auto restricted = syscall(
			SYS_landlock_restrict_self,
			int(ruleset),
			0);
		const auto saved = errno;
		close(int(ruleset));
		if (restricted != 0) {
			errno = saved;
			return Errno("landlock_restrict_self");
		}
		report.landlock = true;
	}
	return report;
}

Result<Report, std::string> Lock(const Options &options, Report report) {
	if ((options.layers & kLayerSeccomp) == 0) {
		return report;
	}
	auto filter = BuildFilter(options);
	if (!filter.ok()) {
		return filter.error();
	}
	auto program = sock_fprog{
		.len = uint16_t(filter.value().size()),
		.filter = filter.value().data(),
	};
	if (syscall(
			SYS_seccomp,
			SECCOMP_SET_MODE_FILTER,
			SECCOMP_FILTER_FLAG_TSYNC,
			&program) != 0) {
		return Errno("seccomp");
	}
	report.seccomp = true;
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
