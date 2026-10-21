#pragma once

#include "manifest.h"
#include "zip_reader.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fgpl {

enum class SignatureState : uint8_t {
	Unsigned,
	Valid,
};

enum class PackageError : uint8_t {
	Archive,
	MissingManifest,
	Manifest,
	Files,
	Content,
	Runtime,
	Signature,
};

struct PackageFailure {
	PackageError error = PackageError::Archive;
	std::vector<std::string> details;
};

struct PackageLimits {
	ZipLimits zip;
	size_t maxManifestBytes = size_t(256) << 10;
	size_t maxIconBytes = size_t(256) << 10;
};

struct Package {
	Manifest manifest;
	cbor::Value document;
	std::string manifestText;
	std::map<std::string, cbor::Bytes> files;
	SignatureState signature = SignatureState::Unsigned;
	std::string publisher;
};

[[nodiscard]] Result<Package, PackageFailure> OpenPackage(
	std::span<const uint8_t> archive,
	const PackageLimits &limits = {});

[[nodiscard]] std::vector<std::pair<std::string, cbor::Bytes>> PluginFiles(
	const Package &package);

[[nodiscard]] Result<std::filesystem::path, std::string> InstallPackage(
	const Package &package,
	const std::filesystem::path &root);

[[nodiscard]] std::string HashText(std::span<const uint8_t> data);

} // namespace fgpl
