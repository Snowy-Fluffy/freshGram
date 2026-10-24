#pragma once

#include "fgpl/result.h"
#include "fgpl/signature.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fgpl {

struct RuntimeBundle {
	int post = 0;
	std::string hash;
	uint64_t size = 0;
};

struct RuntimeIndex {
	int revision = 0;
	int minRevision = 0;
	int host = 0;
	std::string wasmtime;
	std::string python;
	std::vector<int> revoked;
	std::map<std::string, RuntimeBundle> bundles;
};

struct InstalledRuntime {
	int revision = 0;
	int host = 0;
	std::string wasmtime;
	std::string python;
	std::string hostHash;
	std::string pythonHash;
	std::filesystem::path directory;
};

[[nodiscard]] Result<RuntimeIndex, std::string> ParseRuntimeIndex(
	std::string_view text,
	const PublicKey &key);

[[nodiscard]] const RuntimeBundle *BundleFor(
	const RuntimeIndex &index,
	const std::string &platform);

[[nodiscard]] bool Acceptable(const RuntimeIndex &index);

void SaveRuntimePolicy(
	const std::filesystem::path &root,
	const RuntimeIndex &index);

[[nodiscard]] Result<InstalledRuntime, std::string> InstallRuntime(
	std::span<const uint8_t> archive,
	const RuntimeIndex &index,
	const RuntimeBundle &bundle,
	const std::filesystem::path &root);

[[nodiscard]] std::optional<InstalledRuntime> ReadInstalledRuntime(
	const std::filesystem::path &root);

[[nodiscard]] bool VerifyInstalledRuntime(const InstalledRuntime &runtime);

void RemoveRuntime(const std::filesystem::path &root);

} // namespace fgpl
