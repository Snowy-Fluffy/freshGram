#include "ayu/utils/crash_trace.h"

#ifdef Q_OS_LINUX

#include "core/version.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTimer>

#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>

namespace AyuCrashTrace {
namespace {

constexpr auto kFreezeSeconds = 20;
constexpr auto kFramesLimit = 80;

int TraceFd = -1;
pthread_t MainThread;
std::atomic<long long> Heartbeat = 0;
alignas(16) char AlternateStack[64 * 1024];

void WriteTrace(const char *title, int signalNumber) {
	if (TraceFd < 0) {
		return;
	}
	char header[160];
	const auto length = std::snprintf(
		header,
		sizeof(header),
		"\n=== %s, signal %d, time %lld ===\n",
		title,
		signalNumber,
		static_cast<long long>(std::time(nullptr)));
	if (length > 0) {
		[[maybe_unused]] const auto written = ::write(
			TraceFd,
			header,
			length);
	}
	void *frames[kFramesLimit];
	const auto count = ::backtrace(frames, kFramesLimit);
	::backtrace_symbols_fd(frames, count, TraceFd);
}

void CrashHandler(int signalNumber) {
	WriteTrace("crash", signalNumber);
	::signal(signalNumber, SIG_DFL);
	::raise(signalNumber);
}

void FreezeHandler(int signalNumber) {
	WriteTrace("main thread is stuck", signalNumber);
}

void InstallHandler(int signalNumber, void (*handler)(int), int flags) {
	struct sigaction action = {};
	action.sa_handler = handler;
	action.sa_flags = flags;
	::sigemptyset(&action.sa_mask);
	::sigaction(signalNumber, &action, nullptr);
}

void WatchMainThread() {
	auto lastSeen = Heartbeat.load();
	auto stuckFor = 0;
	auto reported = false;
	while (true) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		const auto now = Heartbeat.load();
		if (now != lastSeen) {
			lastSeen = now;
			stuckFor = 0;
			reported = false;
			continue;
		}
		if (++stuckFor >= kFreezeSeconds && !reported) {
			reported = true;
			::pthread_kill(MainThread, SIGUSR2);
		}
	}
}

} // namespace

void Install(const QString &directory) {
	if (TraceFd >= 0) {
		return;
	}
	QDir().mkpath(directory);
	const auto path = QFile::encodeName(directory + QStringLiteral("/crash_trace.txt"));
	const auto flags = O_WRONLY | O_CREAT | O_TRUNC | O_APPEND;
	TraceFd = ::open(path.constData(), flags, 0600);
	if (TraceFd < 0) {
		return;
	}
	// open() does not change the mode of an already existing file, so
	// traces left by older versions are tightened explicitly. If even
	// that fails, give up tracing rather than writing world-readable.
	if (::fchmod(TraceFd, 0600) != 0) {
		::close(TraceFd);
		TraceFd = -1;
		return;
	}
	const auto header = QByteArray("\n=== started, version ")
		+ AppVersionStr
		+ " ===\n";
	[[maybe_unused]] const auto written = ::write(
		TraceFd,
		header.constData(),
		header.size());

	MainThread = ::pthread_self();

	stack_t stack = {};
	stack.ss_sp = AlternateStack;
	stack.ss_size = sizeof(AlternateStack);
	::sigaltstack(&stack, nullptr);

	const auto flagsForCrash = int(SA_ONSTACK | SA_NODEFER);
	for (const auto signalNumber : { SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS }) {
		InstallHandler(signalNumber, CrashHandler, flagsForCrash);
	}
	InstallHandler(SIGUSR2, FreezeHandler, SA_RESTART);

	const auto timer = new QTimer(QCoreApplication::instance());
	QObject::connect(timer, &QTimer::timeout, [] {
		++Heartbeat;
	});
	timer->start(1000);

	std::thread(WatchMainThread).detach();
}

} // namespace AyuCrashTrace

#elif defined Q_OS_WIN

#include "core/version.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QTimer>

#include <windows.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <thread>

namespace AyuCrashTrace {
namespace {

constexpr auto kFreezeSeconds = 20;
constexpr auto kFramesLimit = 80;
constexpr auto kLineLimit = 520;

HANDLE TraceFile = INVALID_HANDLE_VALUE;
HANDLE MainThread = nullptr;
std::atomic<long long> Heartbeat = 0;
std::atomic<bool> Fatal = false;
LPTOP_LEVEL_EXCEPTION_FILTER PreviousFilter = nullptr;
std::uint64_t Frames[kFramesLimit];

void Append(char *buffer, int &length, const char *text) {
	while (*text && length < kLineLimit - 1) {
		buffer[length++] = *text++;
	}
}

void AppendHex(char *buffer, int &length, std::uint64_t value) {
	constexpr auto kDigits = "0123456789abcdef";
	char digits[16];
	auto count = 0;
	do {
		digits[count++] = kDigits[value & 0x0F];
		value >>= 4;
	} while (value && count < 16);
	while (count && length < kLineLimit - 1) {
		buffer[length++] = digits[--count];
	}
}

void AppendDecimal(char *buffer, int &length, std::uint64_t value) {
	char digits[20];
	auto count = 0;
	do {
		digits[count++] = char('0' + (value % 10));
		value /= 10;
	} while (value && count < 20);
	while (count && length < kLineLimit - 1) {
		buffer[length++] = digits[--count];
	}
}

void WriteRaw(const char *data, int length) {
	if (TraceFile == INVALID_HANDLE_VALUE || length <= 0) {
		return;
	}
	DWORD written = 0;
	::WriteFile(TraceFile, data, DWORD(length), &written, nullptr);
}

void WriteHeader(const char *title, std::uint64_t code, std::uint64_t address) {
	char line[kLineLimit];
	auto length = 0;
	Append(line, length, "\n=== ");
	Append(line, length, title);
	Append(line, length, ", code 0x");
	AppendHex(line, length, code);
	Append(line, length, ", address 0x");
	AppendHex(line, length, address);
	Append(line, length, ", time ");
	AppendDecimal(line, length, std::uint64_t(std::time(nullptr)));
	Append(line, length, " ===\n");
	WriteRaw(line, length);
}

void WriteFrame(std::uint64_t address) {
	char line[kLineLimit];
	auto length = 0;
	auto module = HMODULE();
	if (::GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
				| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(address),
			&module) && module) {
		wchar_t path[MAX_PATH];
		const auto size = ::GetModuleFileNameW(module, path, MAX_PATH);
		auto from = DWORD(0);
		for (auto i = DWORD(0); i != size; ++i) {
			if (path[i] == L'\\' || path[i] == L'/') {
				from = i + 1;
			}
		}
		char name[260];
		const auto converted = ::WideCharToMultiByte(
			CP_UTF8,
			0,
			path + from,
			int(size - from),
			name,
			int(sizeof(name)) - 1,
			nullptr,
			nullptr);
		name[converted > 0 ? converted : 0] = 0;
		Append(line, length, name);
		Append(line, length, "+0x");
		AppendHex(
			line,
			length,
			address - reinterpret_cast<std::uint64_t>(module));
	} else {
		Append(line, length, "0x");
		AppendHex(line, length, address);
	}
	Append(line, length, "\n");
	WriteRaw(line, length);
}

int Collect(CONTEXT context) {
	auto count = 0;
#if defined _M_X64 || defined _M_ARM64
	__try {
		while (count < kFramesLimit) {
#ifdef _M_X64
			const auto pc = std::uint64_t(context.Rip);
#else // _M_X64
			const auto pc = std::uint64_t(context.Pc);
#endif // !_M_X64
			if (!pc) {
				break;
			}
			Frames[count++] = pc;
			auto base = DWORD64();
			const auto entry = ::RtlLookupFunctionEntry(pc, &base, nullptr);
			if (entry) {
				PVOID handlerData = nullptr;
				DWORD64 frame = 0;
				::RtlVirtualUnwind(
					UNW_FLAG_NHANDLER,
					base,
					pc,
					entry,
					&context,
					&handlerData,
					&frame,
					nullptr);
			} else {
#ifdef _M_X64
				context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
				context.Rsp += 8;
#else // _M_X64
				break;
#endif // !_M_X64
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
#else // _M_X64 || _M_ARM64
	void *captured[kFramesLimit];
	count = int(::CaptureStackBackTrace(0, kFramesLimit, captured, nullptr));
	for (auto i = 0; i != count; ++i) {
		Frames[i] = reinterpret_cast<std::uint64_t>(captured[i]);
	}
#endif // _M_X64 || _M_ARM64
	return count;
}

void WriteFrames(int count) {
	for (auto i = 0; i != count; ++i) {
		WriteFrame(Frames[i]);
	}
}

void WriteHere(const char *title) {
	CONTEXT context = {};
	::RtlCaptureContext(&context);
	WriteHeader(title, 0, 0);
	WriteFrames(Collect(context));
	::FlushFileBuffers(TraceFile);
}

LONG WINAPI ExceptionFilter(EXCEPTION_POINTERS *info) {
	if (!Fatal.exchange(true)) {
		WriteHeader(
			"crash",
			info->ExceptionRecord->ExceptionCode,
			reinterpret_cast<std::uint64_t>(
				info->ExceptionRecord->ExceptionAddress));
		WriteFrames(Collect(*info->ContextRecord));
		::FlushFileBuffers(TraceFile);
	}
	return PreviousFilter
		? PreviousFilter(info)
		: EXCEPTION_CONTINUE_SEARCH;
}

void TerminateHandler() {
	if (!Fatal.exchange(true)) {
		WriteHere("terminate");
	}
	::TerminateProcess(::GetCurrentProcess(), 3);
}

void AbortHandler(int) {
	if (!Fatal.exchange(true)) {
		WriteHere("abort");
	}
	::signal(SIGABRT, SIG_DFL);
}

void InvalidParameterHandler(
		const wchar_t *,
		const wchar_t *,
		const wchar_t *,
		unsigned int,
		std::uintptr_t) {
	if (!Fatal.exchange(true)) {
		WriteHere("invalid parameter");
	}
	::TerminateProcess(::GetCurrentProcess(), 3);
}

void PureCallHandler() {
	if (!Fatal.exchange(true)) {
		WriteHere("pure virtual call");
	}
	::TerminateProcess(::GetCurrentProcess(), 3);
}

void WriteMainThreadStuck() {
	if (::SuspendThread(MainThread) == DWORD(-1)) {
		return;
	}
	auto count = 0;
	CONTEXT context = {};
	context.ContextFlags = CONTEXT_FULL;
	if (::GetThreadContext(MainThread, &context)) {
		count = Collect(context);
	}
	::ResumeThread(MainThread);
	if (count > 0) {
		WriteHeader("main thread is stuck", 0, 0);
		WriteFrames(count);
		::FlushFileBuffers(TraceFile);
	}
}

void WatchMainThread() {
	auto lastSeen = Heartbeat.load();
	auto stuckFor = 0;
	auto reported = false;
	while (true) {
		::Sleep(1000);
		const auto now = Heartbeat.load();
		if (now != lastSeen) {
			lastSeen = now;
			stuckFor = 0;
			reported = false;
			continue;
		}
		if (++stuckFor >= kFreezeSeconds && !reported) {
			reported = true;
			WriteMainThreadStuck();
		}
	}
}

} // namespace

void Install(const QString &directory) {
	if (TraceFile != INVALID_HANDLE_VALUE) {
		return;
	}
	QDir().mkpath(directory);
	const auto path = QDir::toNativeSeparators(
		directory + QStringLiteral("/crash_trace.txt")).toStdWString();
	TraceFile = ::CreateFileW(
		path.c_str(),
		GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (TraceFile == INVALID_HANDLE_VALUE) {
		return;
	}
	const auto header = QByteArray("\n=== started, version ")
		+ AppVersionStr
		+ " ===\n";
	WriteRaw(header.constData(), int(header.size()));

	::DuplicateHandle(
		::GetCurrentProcess(),
		::GetCurrentThread(),
		::GetCurrentProcess(),
		&MainThread,
		0,
		FALSE,
		DUPLICATE_SAME_ACCESS);

	PreviousFilter = ::SetUnhandledExceptionFilter(ExceptionFilter);
	std::set_terminate(TerminateHandler);
	::signal(SIGABRT, AbortHandler);
	::_set_invalid_parameter_handler(InvalidParameterHandler);
	::_set_purecall_handler(PureCallHandler);

	const auto timer = new QTimer(QCoreApplication::instance());
	QObject::connect(timer, &QTimer::timeout, [] {
		++Heartbeat;
	});
	timer->start(1000);

	if (MainThread) {
		std::thread(WatchMainThread).detach();
	}
}

} // namespace AyuCrashTrace

#else // Q_OS_LINUX || Q_OS_WIN

namespace AyuCrashTrace {

void Install(const QString &directory) {
}

} // namespace AyuCrashTrace

#endif // Q_OS_LINUX || Q_OS_WIN
