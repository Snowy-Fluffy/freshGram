#include "fgpl/package.h"

#include "fgpl/abi.h"
#include "fgpl/base64.h"
#include "fgpl/json.h"
#include "fgpl/sha256.h"
#include "fgpl/wasm_scan.h"

#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <system_error>

namespace fgpl {
namespace {

[[nodiscard]] PackageFailure Fail(
		PackageError error,
		std::vector<std::string> details) {
	return PackageFailure{ error, std::move(details) };
}

[[nodiscard]] bool StartsWith(
		const cbor::Bytes &data,
		std::initializer_list<uint8_t> prefix) {
	return data.size() >= prefix.size()
		&& std::equal(prefix.begin(), prefix.end(), data.begin());
}

[[nodiscard]] std::string Extension(const std::string &path) {
	const auto dot = path.rfind('.');
	const auto slash = path.rfind('/');
	if (dot == std::string::npos
		|| (slash != std::string::npos && dot < slash)) {
		return {};
	}
	auto result = path.substr(dot);
	for (auto &ch : result) {
		if (ch >= 'A' && ch <= 'Z') {
			ch = char(ch - 'A' + 'a');
		}
	}
	return result;
}

[[nodiscard]] std::string ExecutableReason(
		const std::string &path,
		const cbor::Bytes &data) {
	static const auto forbidden = std::set<std::string>{
		".so", ".dll", ".exe", ".dylib", ".dex", ".jar", ".class", ".apk",
		".sh", ".bat", ".cmd", ".ps1", ".com", ".scr", ".msi", ".node",
		".pyd", ".pyc", ".pyo", ".elf", ".bin.exe", ".app", ".vbs", ".js.exe",
	};
	if (forbidden.contains(Extension(path))) {
		return "a native or script file type";
	} else if (StartsWith(data, { 0x7F, 'E', 'L', 'F' })) {
		return "an ELF executable";
	} else if (StartsWith(data, { 'M', 'Z' })) {
		return "a Windows executable";
	} else if (StartsWith(data, { 0xFE, 0xED, 0xFA, 0xCE })
		|| StartsWith(data, { 0xFE, 0xED, 0xFA, 0xCF })
		|| StartsWith(data, { 0xCE, 0xFA, 0xED, 0xFE })
		|| StartsWith(data, { 0xCF, 0xFA, 0xED, 0xFE })
		|| StartsWith(data, { 0xCA, 0xFE, 0xBA, 0xBE })) {
		return "a Mach-O or Java class file";
	} else if (StartsWith(data, { 'd', 'e', 'x', '\n' })) {
		return "a DEX file";
	} else if (StartsWith(data, { '#', '!' }) && Extension(path) != ".py") {
		return "a script with a shebang";
	} else if (StartsWith(data, { 'P', 'K', 3, 4 })) {
		return "a nested archive";
	}
	return {};
}

} // namespace

std::string HashText(std::span<const uint8_t> data) {
	return "sha256-" + Base64Encode(Sha256(data));
}

Result<Package, PackageFailure> OpenPackage(
		std::span<const uint8_t> archive,
		const PackageLimits &limits) {
	auto zip = ReadZip(archive, limits.zip);
	if (!zip.ok()) {
		return Fail(PackageError::Archive, { zip.error().detail });
	}
	auto package = Package();
	auto manifestIndex = std::optional<size_t>();
	for (auto i = size_t(0); i != zip.value().size(); ++i) {
		if (zip.value()[i].name == "manifest.json") {
			manifestIndex = i;
		}
	}
	if (!manifestIndex) {
		return Fail(PackageError::MissingManifest, { "manifest.json is missing" });
	}
	auto &manifestFile = zip.value()[*manifestIndex];
	if (manifestFile.data.size() > limits.maxManifestBytes) {
		return Fail(PackageError::Manifest, { "manifest.json is too large" });
	}
	package.manifestText.assign(
		reinterpret_cast<const char *>(manifestFile.data.data()),
		manifestFile.data.size());
	auto limitsJson = json::Limits();
	limitsJson.maxBytes = limits.maxManifestBytes;
	auto document = json::Parse(package.manifestText, limitsJson);
	if (!document.ok()) {
		return Fail(PackageError::Manifest, { document.error() });
	}
	auto manifest = ParseManifest(document.value());
	if (!manifest.ok()) {
		return Fail(PackageError::Manifest, manifest.error());
	}
	package.manifest = std::move(manifest.value());
	package.document = std::move(document.value());

	auto problems = std::vector<std::string>();
	for (auto &file : zip.value()) {
		if (file.name == "manifest.json") {
			continue;
		}
		const auto listed = package.manifest.files.find(file.name);
		if (listed == package.manifest.files.end()) {
			problems.push_back("'" + file.name + "' is not listed in the manifest");
		} else if (listed->second != HashText(file.data)) {
			problems.push_back("'" + file.name + "' does not match its hash");
		}
		package.files.emplace(file.name, std::move(file.data));
	}
	for (const auto &entry : package.manifest.files) {
		if (!package.files.contains(entry.first)) {
			problems.push_back("'" + entry.first + "' is listed but missing");
		}
	}
	if (!problems.empty()) {
		return Fail(PackageError::Files, std::move(problems));
	}

	for (const auto &file : package.files) {
		const auto reason = ExecutableReason(file.first, file.second);
		if (!reason.empty()) {
			problems.push_back("'" + file.first + "' is " + reason);
		}
	}
	if (!package.manifest.icon.empty()) {
		const auto &icon = package.files.at(package.manifest.icon);
		if (icon.size() > limits.maxIconBytes
			|| !StartsWith(icon, { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A })) {
			problems.push_back("the icon must be a PNG of at most "
				+ std::to_string(limits.maxIconBytes / 1024) + " KiB");
		}
	}
	if (!problems.empty()) {
		return Fail(PackageError::Content, std::move(problems));
	}

	if (package.manifest.kind == "wasm") {
		const auto &module = package.files.at(package.manifest.module);
		const auto info = ScanWasm(module);
		if (!info.ok()) {
			problems.push_back("'" + package.manifest.module + "': " + info.error());
		} else {
			for (const auto &import : info.value().imports) {
				if (!IsAllowedImport(import.first, import.second)) {
					problems.push_back("the module imports '" + import.first
						+ "::" + import.second + "', which is not allowed");
				}
			}
			const auto &exports = info.value().exports;
			const auto reactor = exports.contains("fg_alloc")
				&& exports.contains("fg_init")
				&& exports.contains("fg_on_event");
			const auto command = exports.contains("_start");
			if (!exports.contains("memory") || (!reactor && !command)) {
				problems.push_back("the module must export 'memory' and either "
					"fg_alloc, fg_init and fg_on_event, or _start");
			}
		}
	} else {
		const auto &source = package.files.at(package.manifest.entry);
		if (!cbor::IsValidUtf8(std::string_view(
				reinterpret_cast<const char *>(source.data()),
				source.size()))
			|| std::find(source.begin(), source.end(), uint8_t(0)) != source.end()) {
			problems.push_back("the Python entry file must be UTF-8 text");
		}
	}
	if (!problems.empty()) {
		return Fail(PackageError::Runtime, std::move(problems));
	}

	if (package.manifest.signature) {
		const auto text = SignedText(package.document);
		const auto bytes = std::span<const uint8_t>(
			reinterpret_cast<const uint8_t *>(text.data()),
			text.size());
		if (!Ed25519Verify(
				package.manifest.signature->key,
				bytes,
				package.manifest.signature->value)) {
			return Fail(PackageError::Signature, { "the signature is not valid" });
		}
		package.signature = SignatureState::Valid;
		package.publisher = Fingerprint(package.manifest.signature->key);
	}
	return package;
}

std::vector<std::pair<std::string, cbor::Bytes>> PluginFiles(
		const Package &package) {
	auto result = std::vector<std::pair<std::string, cbor::Bytes>>();
	for (const auto &file : package.files) {
		result.emplace_back("plugin/" + file.first, file.second);
	}
	return result;
}

Result<std::filesystem::path, std::string> InstallPackage(
		const Package &package,
		const std::filesystem::path &root) {
	namespace fs = std::filesystem;
	const auto target = root / package.manifest.id / package.manifest.version.text();
	const auto staging = root / package.manifest.id
		/ (".staging-" + std::to_string(getpid()));
	auto error = std::error_code();
	fs::remove_all(staging, error);
	fs::create_directories(staging, error);
	if (error) {
		return "cannot create " + staging.string();
	}
	const auto write = [&](const fs::path &path, const char *data, size_t size) {
		fs::create_directories(path.parent_path(), error);
		auto stream = std::ofstream(path, std::ios::binary | std::ios::trunc);
		return !error && stream.write(data, std::streamsize(size)).good();
	};
	if (!write(
			staging / "manifest.json",
			package.manifestText.data(),
			package.manifestText.size())) {
		fs::remove_all(staging, error);
		return std::string("cannot write the manifest");
	}
	for (const auto &file : package.files) {
		if (!write(
				staging / file.first,
				reinterpret_cast<const char *>(file.second.data()),
				file.second.size())) {
			fs::remove_all(staging, error);
			return "cannot write " + file.first;
		}
	}
	fs::remove_all(target, error);
	fs::rename(staging, target, error);
	if (error) {
		fs::remove_all(staging, error);
		return std::string("cannot move the package into place");
	}
	return target;
}

} // namespace fgpl
