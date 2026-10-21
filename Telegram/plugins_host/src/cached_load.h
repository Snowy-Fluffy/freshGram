#pragma once

#include "host_client.h"
#include "module_cache.h"

#include <filesystem>

namespace fgpl {

enum class ModuleSource : uint8_t {
	Cache,
	Compiled,
	Direct,
};

struct CachedLoadResult {
	ModuleSource source = ModuleSource::Direct;
	bool cacheRejected = false;
};

struct CacheSettings {
	std::filesystem::path directory;
	std::array<uint8_t, 32> secret = {};
	uint64_t maxTotalBytes = uint64_t(256) << 20;
};

[[nodiscard]] Result<CachedLoadResult, ClientFailure> LoadWithCache(
	HostClient &client,
	const HostConfig &compilerConfig,
	LoadRequest request,
	const CacheSettings &settings,
	const std::atomic<bool> *cancel = nullptr);

} // namespace fgpl
