#include "fgpl/directory_package.h"

#include "fgpl/json.h"
#include "fgpl/package.h"
#include "fgpl/zip_writer.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace fgpl {

namespace fs = std::filesystem;

std::optional<cbor::Bytes> ReadWholeFile(const fs::path &path) {
	auto stream = std::ifstream(path, std::ios::binary);
	if (!stream) {
		return std::nullopt;
	}
	return cbor::Bytes(std::istreambuf_iterator<char>(stream), {});
}

Result<DirectoryPackage, std::string> ReadDirectoryPackage(const fs::path &directory) {
	const auto manifestBytes = ReadWholeFile(directory / "manifest.json");
	if (!manifestBytes) {
		return "no manifest.json in " + directory.string();
	}
	auto document = json::Parse(std::string(manifestBytes->begin(), manifestBytes->end()));
	if (!document.ok() || !document.value().asMap()) {
		return "manifest.json: " + (document.ok() ? std::string("not an object") : document.error());
	}
	auto result = DirectoryPackage();
	for (const auto &entry : fs::recursive_directory_iterator(directory)) {
		const auto relative = entry.path().lexically_relative(directory).generic_string();
		if (entry.is_symlink()) {
			return "symbolic links are not packed: " + relative;
		} else if (!entry.is_regular_file()) {
			continue;
		}
		if (relative == "manifest.json") {
			continue;
		}
		auto content = ReadWholeFile(entry.path());
		if (!content) {
			return "cannot read " + relative;
		}
		result.files.emplace_back(relative, std::move(*content));
	}
	std::sort(result.files.begin(), result.files.end(), [](const auto &a, const auto &b) {
		return a.first < b.first;
	});
	auto hashes = cbor::Map();
	for (const auto &file : result.files) {
		hashes.push_back({ file.first, cbor::Value::FromText(HashText(file.second)) });
	}
	auto map = cbor::Map();
	for (auto &entry : *document.value().asMap()) {
		if (entry.key != "files" && entry.key != "signature") {
			map.push_back(std::move(entry));
		}
	}
	map.push_back({ "files", cbor::Value::FromMap(std::move(hashes)) });
	result.document = cbor::Value::FromMap(std::move(map));
	return result;
}

cbor::Bytes WriteArchive(const cbor::Value &document, PackageFiles files) {
	const auto manifestText = json::Canonical(document);
	auto archiveFiles = PackageFiles();
	archiveFiles.emplace_back(
		"manifest.json",
		cbor::Bytes(manifestText.begin(), manifestText.end()));
	for (auto &file : files) {
		archiveFiles.push_back(std::move(file));
	}
	return WriteZip(archiveFiles);
}

} // namespace fgpl
