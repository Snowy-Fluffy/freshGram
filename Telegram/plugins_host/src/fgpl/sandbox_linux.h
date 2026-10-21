#pragma once

#include "fgpl/result.h"

#include <cstdint>
#include <string>

namespace fgpl::sandbox {

constexpr auto kLayerRlimits = uint32_t(1);
constexpr auto kLayerLandlock = uint32_t(2);
constexpr auto kLayerSeccomp = uint32_t(4);
constexpr auto kLayersAll = kLayerRlimits | kLayerLandlock | kLayerSeccomp;

struct Options {
	int ipcFd = -1;
	uint32_t layers = kLayersAll;
	bool requireLandlock = false;
	bool allowStderr = true;
	int maxOpenFiles = 8;
	uint64_t addressSpaceBytes = 0;
	uint64_t cpuSeconds = 0;
};

struct Report {
	int landlockAbi = 0;
	bool rlimits = false;
	bool landlock = false;
	bool seccomp = false;
};

[[nodiscard]] int LandlockAbi();

[[nodiscard]] Result<Report, std::string> Restrict(
	const Options &options,
	Report report = {});

[[nodiscard]] Result<Report, std::string> Lock(
	const Options &options,
	Report report = {});

[[nodiscard]] Result<Report, std::string> Apply(const Options &options);

} // namespace fgpl::sandbox
