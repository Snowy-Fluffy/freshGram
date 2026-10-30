#pragma once

#include "fgpl/os.h"
#include "fgpl/result.h"

#include <cstdint>
#include <string>

namespace fgpl::sandbox {

constexpr auto kLayerRlimits = uint32_t(1);
constexpr auto kLayerLandlock = uint32_t(2);
constexpr auto kLayerSeccomp = uint32_t(4);
constexpr auto kLayersAll = kLayerRlimits | kLayerLandlock | kLayerSeccomp;

struct Options {
	os::Handle ipcFd = os::kNoHandle;
	uint32_t layers = kLayersAll;
	bool allowStderr = true;
	int maxOpenFiles = 8;
	uint64_t addressSpaceBytes = 0;
	uint64_t cpuSeconds = 0;
};

// What the host could enforce on itself. The three generic flags are all
// the client needs, the rest is the Linux detail kept for its tests.
struct Report {
	bool filesystem = false;
	bool processes = false;
	bool limits = false;

	int landlockAbi = 0;
	bool rlimits = false;
	bool landlock = false;
	bool seccomp = false;

	[[nodiscard]] bool complete() const {
		return filesystem && processes && limits;
	}
};

// Whether this system can run the host in its sandbox at all.
[[nodiscard]] bool Supported();

[[nodiscard]] Result<Report, std::string> Restrict(
	const Options &options,
	Report report = {});

[[nodiscard]] Result<Report, std::string> Lock(
	const Options &options,
	Report report = {});

[[nodiscard]] Result<Report, std::string> Apply(const Options &options);

} // namespace fgpl::sandbox
