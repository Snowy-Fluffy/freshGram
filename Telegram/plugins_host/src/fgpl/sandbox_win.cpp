#include "fgpl/sandbox.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace fgpl::sandbox {
namespace {

[[nodiscard]] bool InAppContainer() {
	auto token = HANDLE();
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		return false;
	}
	auto flag = DWORD(0);
	auto size = DWORD(0);
	const auto ok = GetTokenInformation(
		token,
		TokenIsAppContainer,
		&flag,
		sizeof(flag),
		&size);
	CloseHandle(token);
	return ok && flag != 0;
}

// The parent puts the host into its job before it runs, the job is what
// bounds memory and CPU time and keeps the host from leaving the process count.
[[nodiscard]] bool JobEnforces(const Options &options, bool &limits) {
	auto inJob = BOOL(FALSE);
	if (!IsProcessInJob(GetCurrentProcess(), nullptr, &inJob) || !inJob) {
		return false;
	}
	auto info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
	if (!QueryInformationJobObject(
			nullptr,
			JobObjectExtendedLimitInformation,
			&info,
			sizeof(info),
			nullptr)) {
		return false;
	}
	const auto flags = info.BasicLimitInformation.LimitFlags;
	const auto need = [&](DWORD flag) {
		return (flags & flag) == flag;
	};
	limits = need(JOB_OBJECT_LIMIT_ACTIVE_PROCESS)
		&& need(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)
		&& (!options.addressSpaceBytes || need(JOB_OBJECT_LIMIT_PROCESS_MEMORY))
		&& (!options.cpuSeconds || need(JOB_OBJECT_LIMIT_PROCESS_TIME));
	return info.BasicLimitInformation.ActiveProcessLimit == 1;
}

} // namespace

bool Supported() {
	const auto library = LoadLibraryExW(
		L"userenv.dll",
		nullptr,
		LOAD_LIBRARY_SEARCH_SYSTEM32);
	return library
		&& GetProcAddress(library, "CreateAppContainerProfile")
		&& GetProcAddress(library, "DeriveAppContainerSidFromAppContainerName");
}

Result<Report, std::string> Restrict(const Options &options, Report report) {
	SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
	SetErrorMode(
		SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRBOX | SEM_NOOPENFILEERRORBOX);
	if (!options.layers) {
		return report;
	}
	if (!InAppContainer()) {
		return std::string("the host does not run in an AppContainer");
	}
	report.filesystem = true;
	auto limits = false;
	if (!JobEnforces(options, limits)) {
		return std::string("the host does not run in its job");
	}
	report.processes = true;
	report.limits = limits;
	return report;
}

Result<Report, std::string> Lock(const Options &, Report report) {
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
