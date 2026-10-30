#include "fgpl/os.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>

#include <array>
#include <random>
#include <vector>
#include <string>

#ifndef PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES
#define PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES 0x00020009
#endif
#ifndef PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY
#define PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY 0x0002000E
#endif
#ifndef PROCESS_CREATION_CHILD_PROCESS_RESTRICTED
#define PROCESS_CREATION_CHILD_PROCESS_RESTRICTED 0x01
#endif

namespace fgpl::os {
namespace {

constexpr auto kContainerName = L"freshGram.PluginHost";

// Older than Windows 8 has no AppContainer, so the functions are looked up.
using CreateProfileFn = HRESULT (WINAPI *)(
	PCWSTR,
	PCWSTR,
	PCWSTR,
	PSID_AND_ATTRIBUTES,
	DWORD,
	PSID *);
using DeriveSidFn = HRESULT (WINAPI *)(PCWSTR, PSID *);

struct ContainerApi {
	CreateProfileFn create = nullptr;
	DeriveSidFn derive = nullptr;
};

[[nodiscard]] const ContainerApi &Api() {
	static const auto api = [] {
		auto result = ContainerApi();
		if (const auto library = LoadLibraryExW(
				L"userenv.dll",
				nullptr,
				LOAD_LIBRARY_SEARCH_SYSTEM32)) {
			result.create = reinterpret_cast<CreateProfileFn>(
				GetProcAddress(library, "CreateAppContainerProfile"));
			result.derive = reinterpret_cast<DeriveSidFn>(
				GetProcAddress(library, "DeriveAppContainerSidFromAppContainerName"));
		}
		return result;
	}();
	return api;
}

[[nodiscard]] HANDLE Native(Handle handle) {
	return reinterpret_cast<HANDLE>(handle);
}

[[nodiscard]] std::string Describe(const char *what, DWORD code = GetLastError()) {
	return std::string(what) + " failed (" + std::to_string(code) + ")";
}

[[nodiscard]] std::wstring Widen(const std::string &text) {
	return PathFromUtf8(text).native();
}

// Appends one argument the way CommandLineToArgvW reads it back.
void AppendArgument(std::wstring &line, const std::wstring &argument) {
	if (!line.empty()) {
		line += L' ';
	}
	if (!argument.empty()
		&& argument.find_first_of(L" \t\"") == std::wstring::npos) {
		line += argument;
		return;
	}
	line += L'"';
	for (auto i = size_t(0);; ++i) {
		auto slashes = size_t(0);
		while (i != argument.size() && argument[i] == L'\\') {
			++i;
			++slashes;
		}
		if (i == argument.size()) {
			line.append(slashes * 2, L'\\');
			break;
		} else if (argument[i] == L'"') {
			line.append(slashes * 2 + 1, L'\\');
		} else {
			line.append(slashes, L'\\');
		}
		line += argument[i];
	}
	line += L'"';
}

struct Container {
	PSID sid = nullptr;

	Container() = default;
	Container(const Container &) = delete;
	Container &operator=(const Container &) = delete;
	~Container() {
		if (sid) {
			FreeSid(sid);
		}
	}
};

[[nodiscard]] bool OpenContainer(Container &container, std::string &error) {
	const auto &api = Api();
	if (!api.create || !api.derive) {
		error = "this Windows has no AppContainer";
		return false;
	}
	auto result = api.create(
		kContainerName,
		kContainerName,
		L"freshGram plugin host",
		nullptr,
		0,
		&container.sid);
	if (result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
		result = api.derive(kContainerName, &container.sid);
	}
	if (FAILED(result) || !container.sid) {
		error = Describe("opening the AppContainer", DWORD(result));
		return false;
	}
	return true;
}

// The container reads nothing but the host itself, it needs no other file.
[[nodiscard]] bool GrantRead(
		const std::wstring &file,
		PSID sid,
		std::string &error) {
	auto oldAcl = PACL();
	auto descriptor = PSECURITY_DESCRIPTOR();
	auto code = GetNamedSecurityInfoW(
		file.c_str(),
		SE_FILE_OBJECT,
		DACL_SECURITY_INFORMATION,
		nullptr,
		nullptr,
		&oldAcl,
		nullptr,
		&descriptor);
	if (code != ERROR_SUCCESS) {
		error = Describe("reading the host permissions", code);
		return false;
	}
	auto access = EXPLICIT_ACCESSW();
	access.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
	access.grfAccessMode = GRANT_ACCESS;
	access.grfInheritance = NO_INHERITANCE;
	access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	access.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
	access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
	auto newAcl = PACL();
	code = SetEntriesInAclW(1, &access, oldAcl, &newAcl);
	if (code == ERROR_SUCCESS) {
		code = SetNamedSecurityInfoW(
			const_cast<LPWSTR>(file.c_str()),
			SE_FILE_OBJECT,
			DACL_SECURITY_INFORMATION,
			nullptr,
			nullptr,
			newAcl,
			nullptr);
	}
	if (newAcl) {
		LocalFree(newAcl);
	}
	LocalFree(descriptor);
	if (code != ERROR_SUCCESS) {
		error = Describe("granting the host to the AppContainer", code);
		return false;
	}
	return true;
}

struct Pipe {
	HANDLE server = INVALID_HANDLE_VALUE;
	HANDLE client = INVALID_HANDLE_VALUE;
};

[[nodiscard]] bool MakePipe(Pipe &pipe, std::string &error) {
	auto random = std::random_device();
	const auto name = std::wstring(L"\\\\.\\pipe\\fgpl-")
		+ std::to_wstring(GetCurrentProcessId()) + L"-"
		+ std::to_wstring((uint64_t(random()) << 32) | random());
	pipe.server = CreateNamedPipeW(
		name.c_str(),
		PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
		1,
		1 << 16,
		1 << 16,
		0,
		nullptr);
	if (pipe.server == INVALID_HANDLE_VALUE) {
		error = Describe("creating the channel");
		return false;
	}
	auto inheritable = SECURITY_ATTRIBUTES();
	inheritable.nLength = sizeof(inheritable);
	inheritable.bInheritHandle = TRUE;
	pipe.client = CreateFileW(
		name.c_str(),
		GENERIC_READ | GENERIC_WRITE,
		0,
		&inheritable,
		OPEN_EXISTING,
		FILE_FLAG_OVERLAPPED,
		nullptr);
	if (pipe.client == INVALID_HANDLE_VALUE) {
		error = Describe("connecting the channel");
		CloseHandle(pipe.server);
		pipe.server = INVALID_HANDLE_VALUE;
		return false;
	}
	return true;
}

[[nodiscard]] HANDLE OpenNull() {
	auto inheritable = SECURITY_ATTRIBUTES();
	inheritable.nLength = sizeof(inheritable);
	inheritable.bInheritHandle = TRUE;
	return CreateFileW(
		L"NUL",
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		&inheritable,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
}

[[nodiscard]] HANDLE MakeJob(const SpawnSpec &spec) {
	const auto job = CreateJobObjectW(nullptr, nullptr);
	if (!job) {
		return nullptr;
	}
	auto limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
	auto &basic = limits.BasicLimitInformation;
	basic.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
		| JOB_OBJECT_LIMIT_ACTIVE_PROCESS
		| JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
	basic.ActiveProcessLimit = 1;
	if (spec.addressSpaceMb > 0) {
		// The limit counts committed memory, the address space Wasmtime reserves is free.
		basic.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
		limits.ProcessMemoryLimit = SIZE_T(spec.addressSpaceMb) << 20;
	}
	if (spec.cpuSeconds > 0) {
		basic.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_TIME;
		basic.PerProcessUserTimeLimit.QuadPart = LONGLONG(spec.cpuSeconds) * 10000000;
	}
	auto ui = JOBOBJECT_BASIC_UI_RESTRICTIONS();
	ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP
		| JOB_OBJECT_UILIMIT_DISPLAYSETTINGS
		| JOB_OBJECT_UILIMIT_EXITWINDOWS
		| JOB_OBJECT_UILIMIT_GLOBALATOMS
		| JOB_OBJECT_UILIMIT_HANDLES
		| JOB_OBJECT_UILIMIT_READCLIPBOARD
		| JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS
		| JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
	if (!SetInformationJobObject(
			job,
			JobObjectExtendedLimitInformation,
			&limits,
			sizeof(limits))
		|| !SetInformationJobObject(
			job,
			JobObjectBasicUIRestrictions,
			&ui,
			sizeof(ui))) {
		CloseHandle(job);
		return nullptr;
	}
	return job;
}

constexpr auto kMitigationStrictHandles = DWORD64(0x00000001ULL) << 24;
constexpr auto kMitigationExtensionPoints = DWORD64(0x00000001ULL) << 32;
constexpr auto kMitigationWin32k = DWORD64(0x00000001ULL) << 28;
constexpr auto kMitigationNoRemoteImages = DWORD64(0x00000001ULL) << 52;
constexpr auto kMitigationNoLowLabelImages = DWORD64(0x00000001ULL) << 56;
constexpr auto kMitigationPreferSystem32 = DWORD64(0x00000001ULL) << 60;

} // namespace

struct Child::Native {
	HANDLE process = nullptr;
	HANDLE job = nullptr;
	DWORD pid = 0;
	bool reaped = false;
	DWORD exitCode = 0;
};

Child::Child(std::unique_ptr<Native> native)
: _native(std::move(native)) {
}

Child::~Child() {
	if (_native->process) {
		CloseHandle(_native->process);
	}
	if (_native->job) {
		CloseHandle(_native->job);
	}
}

Result<std::unique_ptr<Child>, std::string> Child::Spawn(
		const SpawnSpec &spec,
		Handle &channel) {
	auto error = std::string();
	auto container = Container();
	const auto path = Widen(spec.path);
	if (spec.sandboxed) {
		if (!OpenContainer(container, error)
			|| !GrantRead(path, container.sid, error)) {
			return error;
		}
	}
	auto pipe = Pipe();
	if (!MakePipe(pipe, error)) {
		return error;
	}
	const auto nul = OpenNull();
	if (nul == INVALID_HANDLE_VALUE) {
		CloseHandle(pipe.server);
		CloseHandle(pipe.client);
		return Describe("opening NUL");
	}
	const auto job = MakeJob(spec);
	const auto cleanup = [&] {
		CloseHandle(pipe.client);
		CloseHandle(nul);
		if (job) {
			CloseHandle(job);
		}
	};
	if (!job) {
		const auto text = Describe("creating the job");
		CloseHandle(pipe.server);
		cleanup();
		return text;
	}

	auto line = std::wstring();
	AppendArgument(line, path);
	AppendArgument(line, L"--ipc-fd=" + std::to_wstring(
		reinterpret_cast<uintptr_t>(pipe.client)));
	for (const auto &argument : spec.arguments) {
		AppendArgument(line, Widen(argument));
	}
	line.push_back(L'\0');

	// Stdio goes to NUL, the pipe is the only other handle the host gets.
	auto inherited = std::array<HANDLE, 2>{ pipe.client, nul };
	auto policy = DWORD(PROCESS_CREATION_CHILD_PROCESS_RESTRICTED);
	auto mitigations = DWORD64(kMitigationStrictHandles
		| kMitigationExtensionPoints
		| kMitigationNoRemoteImages
		| kMitigationNoLowLabelImages
		| kMitigationPreferSystem32
		| kMitigationWin32k);
	auto capabilities = SECURITY_CAPABILITIES();
	capabilities.AppContainerSid = container.sid;

	auto info = PROCESS_INFORMATION();
	auto created = false;
	auto code = DWORD(0);
	// The mitigations are hardening on top of the container, a Windows that
	// refuses them still runs the host in it.
	for (auto withMitigations : { true, false }) {
		auto size = SIZE_T(0);
		InitializeProcThreadAttributeList(nullptr, 4, 0, &size);
		auto storage = std::vector<uint8_t>(size);
		const auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
		if (!InitializeProcThreadAttributeList(list, 4, 0, &size)) {
			code = GetLastError();
			break;
		}
		auto ok = UpdateProcThreadAttribute(
			list,
			0,
			PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
			inherited.data(),
			sizeof(HANDLE) * inherited.size(),
			nullptr,
			nullptr) != FALSE;
		ok = ok && UpdateProcThreadAttribute(
			list,
			0,
			PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY,
			&policy,
			sizeof(policy),
			nullptr,
			nullptr);
		if (ok && spec.sandboxed) {
			ok = UpdateProcThreadAttribute(
				list,
				0,
				PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
				&capabilities,
				sizeof(capabilities),
				nullptr,
				nullptr) != FALSE;
		}
		if (ok && withMitigations) {
			ok = UpdateProcThreadAttribute(
				list,
				0,
				PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
				&mitigations,
				sizeof(mitigations),
				nullptr,
				nullptr) != FALSE;
		}
		if (!ok) {
			code = GetLastError();
			DeleteProcThreadAttributeList(list);
			continue;
		}
		auto startup = STARTUPINFOEXW();
		startup.StartupInfo.cb = sizeof(startup);
		startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		startup.StartupInfo.hStdInput = nul;
		startup.StartupInfo.hStdOutput = nul;
		startup.StartupInfo.hStdError = nul;
		startup.lpAttributeList = list;
		// An empty environment: two terminating zeros.
		wchar_t environment[2] = {};
		created = CreateProcessW(
			path.c_str(),
			line.data(),
			nullptr,
			nullptr,
			TRUE,
			EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW
				| CREATE_UNICODE_ENVIRONMENT,
			environment,
			nullptr,
			&startup.StartupInfo,
			&info) != FALSE;
		code = created ? 0 : GetLastError();
		DeleteProcThreadAttributeList(list);
		if (created) {
			break;
		}
	}
	if (!created) {
		CloseHandle(pipe.server);
		cleanup();
		return Describe("starting the host", code);
	}
	if (!AssignProcessToJobObject(job, info.hProcess)) {
		const auto text = Describe("assigning the host to its job");
		TerminateProcess(info.hProcess, 1);
		CloseHandle(info.hThread);
		CloseHandle(info.hProcess);
		CloseHandle(pipe.server);
		cleanup();
		return text;
	}
	ResumeThread(info.hThread);
	CloseHandle(info.hThread);
	CloseHandle(pipe.client);
	CloseHandle(nul);

	channel = reinterpret_cast<Handle>(pipe.server);
	if (spec.sendTimeoutMs > 0) {
		SetSendTimeout(channel, spec.sendTimeoutMs);
	}
	auto native = std::make_unique<Native>();
	native->process = info.hProcess;
	native->job = job;
	native->pid = info.dwProcessId;
	return std::unique_ptr<Child>(new Child(std::move(native)));
}

int Child::id() const {
	return int(_native->pid);
}

void Child::terminate() {
	if (_native->process && !_native->reaped) {
		TerminateProcess(_native->process, 1);
	}
}

namespace {

void Convert(DWORD code, ExitInfo &info) {
	info = ExitInfo();
	// Exception codes (0xC0000005 and alike) are what a crash looks like here.
	if (code >= 0xC0000000UL) {
		info.signaled = true;
		info.signal = int(code);
	} else {
		info.code = int(code);
	}
}

} // namespace

bool Child::tryWait(ExitInfo &info) {
	if (_native->reaped) {
		return false;
	}
	if (WaitForSingleObject(_native->process, 0) != WAIT_OBJECT_0
		|| !GetExitCodeProcess(_native->process, &_native->exitCode)) {
		return false;
	}
	_native->reaped = true;
	Convert(_native->exitCode, info);
	return true;
}

void Child::wait(ExitInfo &info) {
	if (_native->reaped) {
		return;
	}
	WaitForSingleObject(_native->process, INFINITE);
	if (GetExitCodeProcess(_native->process, &_native->exitCode)) {
		_native->reaped = true;
		Convert(_native->exitCode, info);
	}
}

int SandboxViolationSignal() {
	return 0;
}

} // namespace fgpl::os
