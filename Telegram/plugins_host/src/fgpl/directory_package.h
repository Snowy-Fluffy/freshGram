#pragma once

#include "fgpl/cbor.h"
#include "fgpl/result.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fgpl {

using PackageFiles = std::vector<std::pair<std::string, cbor::Bytes>>;

struct DirectoryPackage {
	// The manifest with its `files` hashes filled in and no signature.
	cbor::Value document;
	PackageFiles files;
};

[[nodiscard]] std::optional<cbor::Bytes> ReadWholeFile(
	const std::filesystem::path &path);

// Reads manifest.json and every other file of a plugin directory.
[[nodiscard]] Result<DirectoryPackage, std::string> ReadDirectoryPackage(
	const std::filesystem::path &directory);

// The zip of a package; the document may already carry a signature.
[[nodiscard]] cbor::Bytes WriteArchive(
	const cbor::Value &document,
	PackageFiles files);

} // namespace fgpl
