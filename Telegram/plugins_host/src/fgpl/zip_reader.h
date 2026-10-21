#pragma once

#include "fgpl/cbor.h"
#include "fgpl/result.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fgpl {

struct ZipLimits {
	size_t maxEntries = 4096;
	uint64_t maxEntryBytes = uint64_t(128) << 20;
	uint64_t maxTotalBytes = uint64_t(256) << 20;
	uint64_t maxRatio = 100;
	size_t maxNameBytes = 255;
	size_t maxDepth = 16;
	uint64_t maxArchiveBytes = uint64_t(300) << 20;
};

enum class ZipError : uint8_t {
	Format,
	Unsupported,
	Unsafe,
	TooLarge,
	Corrupt,
};

struct ZipFailure {
	ZipError error = ZipError::Format;
	std::string detail;
};

struct ZipFile {
	std::string name;
	cbor::Bytes data;
};

[[nodiscard]] Result<std::vector<ZipFile>, ZipFailure> ReadZip(
	std::span<const uint8_t> archive,
	const ZipLimits &limits = {});

} // namespace fgpl
