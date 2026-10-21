#include "cached_load.h"

#include <string>

namespace fgpl {
namespace {

[[nodiscard]] std::string Fingerprint(
		const HostClient &client,
		const HostConfig &config) {
	auto result = "engine=" + client.sandbox().engine + ";format=1";
	for (const auto &argument : config.extraArguments) {
		result += ";" + argument;
	}
	result += ";reservation=" + std::to_string(config.reservationMb);
	return result;
}

} // namespace

Result<CachedLoadResult, ClientFailure> LoadWithCache(
		HostClient &client,
		const HostConfig &compilerConfig,
		LoadRequest request,
		const CacheSettings &settings) {
	auto key = CacheKey();
	key.secret = settings.secret;
	key.fingerprint = Fingerprint(client, compilerConfig);
	const auto cache = ModuleCache(settings.directory, std::move(key));
	auto result = CachedLoadResult();
	const auto wasm = request.wasm;

	const auto direct = [&]() -> Result<CachedLoadResult, ClientFailure> {
		request.compiled.clear();
		request.wasm = wasm;
		const auto loaded = client.load(request);
		if (!loaded.ok()) {
			return loaded.error();
		}
		result.source = ModuleSource::Direct;
		return result;
	};

	if (auto hit = cache.find(wasm)) {
		request.compiled = std::move(*hit);
		request.wasm.clear();
		const auto loaded = client.load(request);
		if (loaded.ok()) {
			result.source = ModuleSource::Cache;
			return result;
		} else if (loaded.error().error != ClientError::LoadFailed) {
			return loaded.error();
		}
		cache.erase(wasm);
		result.cacheRejected = true;
		return direct();
	}

	auto compiled = HostClient::Compile(compilerConfig, wasm);
	if (!compiled.ok()) {
		return direct();
	}
	if (cache.store(wasm, compiled.value())) {
		cache.prune(settings.maxTotalBytes);
	}
	request.compiled = std::move(compiled.value());
	request.wasm.clear();
	const auto loaded = client.load(request);
	if (loaded.ok()) {
		result.source = ModuleSource::Compiled;
		return result;
	} else if (loaded.error().error != ClientError::LoadFailed) {
		return loaded.error();
	}
	cache.erase(wasm);
	result.cacheRejected = true;
	return direct();
}

} // namespace fgpl
