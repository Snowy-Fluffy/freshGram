#pragma once

#include "sha256.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fgpl {

struct CacheKey {
	std::array<uint8_t, 32> secret = {};
	std::string fingerprint;
};

class ModuleCache {
public:
	ModuleCache(std::filesystem::path directory, CacheKey key);

	[[nodiscard]] std::optional<std::vector<uint8_t>> find(
		std::span<const uint8_t> wasm) const;
	[[nodiscard]] bool store(
		std::span<const uint8_t> wasm,
		std::span<const uint8_t> compiled) const;
	void erase(std::span<const uint8_t> wasm) const;
	void prune(uint64_t maxTotalBytes) const;

	[[nodiscard]] std::filesystem::path pathFor(
		std::span<const uint8_t> wasm) const;

private:
	[[nodiscard]] Digest seal(std::span<const uint8_t> content) const;

	std::filesystem::path _directory;
	CacheKey _key;

};

} // namespace fgpl
