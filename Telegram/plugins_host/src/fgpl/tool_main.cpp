#include "fgpl/base64.h"
#include "fgpl/json.h"
#include "fgpl/package.h"
#include "fgpl/permissions.h"
#include "fgpl/runtime_bundle.h"
#include "fgpl/zip_writer.h"

#include <openssl/rand.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

using namespace fgpl;
namespace fs = std::filesystem;

[[nodiscard]] std::optional<cbor::Bytes> ReadFile(const fs::path &path) {
	auto stream = std::ifstream(path, std::ios::binary);
	if (!stream) {
		return std::nullopt;
	}
	return cbor::Bytes(std::istreambuf_iterator<char>(stream), {});
}

[[nodiscard]] bool WriteFile(const fs::path &path, const std::string &text) {
	auto stream = std::ofstream(path, std::ios::binary | std::ios::trunc);
	return stream.write(text.data(), std::streamsize(text.size())).good();
}

[[nodiscard]] std::optional<PrivateSeed> ReadSeed(const std::string &path) {
	const auto bytes = ReadFile(path);
	if (!bytes) {
		return std::nullopt;
	}
	auto text = std::string(bytes->begin(), bytes->end());
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
		text.pop_back();
	}
	const auto decoded = Base64Decode(text);
	if (!decoded || decoded->size() != 32) {
		return std::nullopt;
	}
	auto seed = PrivateSeed();
	std::copy(decoded->begin(), decoded->end(), seed.begin());
	return seed;
}

int KeyGen(const std::string &name) {
	auto seed = PrivateSeed();
	if (RAND_bytes(seed.data(), int(seed.size())) != 1) {
		std::fprintf(stderr, "no random bytes\n");
		return 1;
	}
	const auto key = Ed25519PublicKey(seed);
	if (!key || !WriteFile(name + ".key", Base64Encode(seed) + "\n")) {
		std::fprintf(stderr, "cannot write %s.key\n", name.c_str());
		return 1;
	}
	std::printf("private key: %s.key (keep it secret)\n", name.c_str());
	std::printf("public key:  %s\n", Base64Encode(*key).c_str());
	std::printf("fingerprint: %s\n", Fingerprint(*key).c_str());
	return 0;
}

int Describe(const Package &package) {
	const auto &manifest = package.manifest;
	std::printf("%s %s (%s)\n", manifest.id.c_str(), manifest.version.text().c_str(), manifest.name.get("default").c_str());
	std::printf("runtime:   %s\n", manifest.kind.c_str());
	std::printf("works with freshGram %s\n", manifest.appRange.text().c_str());
	if (package.signature == SignatureState::Valid) {
		std::printf("signature: valid, publisher %s\n", package.publisher.c_str());
	} else {
		std::printf("signature: none (unverified publisher)\n");
	}
	auto granted = std::set<std::string>();
	for (const auto &request : manifest.permissions) {
		std::printf(
			"permission: %s (%s) - %s\n",
			request.id.c_str(),
			request.required ? "required" : "optional",
			request.reason.get("default").c_str());
		granted.insert(request.id);
	}
	for (const auto &note : RiskNotes(granted)) {
		std::printf("risk:      %s\n", note.text.c_str());
	}
	std::printf("files:     %zu\n", package.files.size());
	return 0;
}

int Verify(const std::string &path) {
	const auto bytes = ReadFile(path);
	if (!bytes) {
		std::fprintf(stderr, "cannot read %s\n", path.c_str());
		return 1;
	}
	const auto opened = OpenPackage(*bytes);
	if (!opened.ok()) {
		std::fprintf(stderr, "the package is not valid:\n");
		for (const auto &line : opened.error().details) {
			std::fprintf(stderr, "  %s\n", line.c_str());
		}
		return 2;
	}
	return Describe(opened.value());
}

int Pack(const fs::path &directory, const std::string &output, const std::string &keyPath) {
	const auto manifestBytes = ReadFile(directory / "manifest.json");
	if (!manifestBytes) {
		std::fprintf(stderr, "no manifest.json in %s\n", directory.string().c_str());
		return 1;
	}
	auto document = json::Parse(std::string(manifestBytes->begin(), manifestBytes->end()));
	if (!document.ok() || !document.value().asMap()) {
		std::fprintf(stderr, "manifest.json: %s\n", document.ok() ? "not an object" : document.error().c_str());
		return 1;
	}
	auto files = std::vector<std::pair<std::string, cbor::Bytes>>();
	for (const auto &entry : fs::recursive_directory_iterator(directory)) {
		if (!entry.is_regular_file()) {
			continue;
		}
		const auto relative = fs::relative(entry.path(), directory).generic_string();
		if (relative == "manifest.json") {
			continue;
		}
		auto content = ReadFile(entry.path());
		if (!content) {
			std::fprintf(stderr, "cannot read %s\n", relative.c_str());
			return 1;
		}
		files.emplace_back(relative, std::move(*content));
	}
	std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) {
		return a.first < b.first;
	});
	auto hashes = cbor::Map();
	for (const auto &file : files) {
		hashes.push_back({ file.first, cbor::Value::FromText(HashText(file.second)) });
	}
	auto map = cbor::Map();
	for (auto &entry : *document.value().asMap()) {
		if (entry.key != "files" && entry.key != "signature") {
			map.push_back(std::move(entry));
		}
	}
	map.push_back({ "files", cbor::Value::FromMap(std::move(hashes)) });
	auto signedDocument = cbor::Value::FromMap(std::move(map));
	if (!keyPath.empty()) {
		const auto seed = ReadSeed(keyPath);
		const auto key = seed ? Ed25519PublicKey(*seed) : std::nullopt;
		if (!key) {
			std::fprintf(stderr, "cannot use the key %s\n", keyPath.c_str());
			return 1;
		}
		const auto text = SignedText(signedDocument);
		const auto signature = Ed25519Sign(*seed, {
			reinterpret_cast<const uint8_t *>(text.data()), text.size() });
		signedDocument.asMap()->push_back({ "signature", cbor::Value::FromMap({
			{ "alg", cbor::Value::FromText("ed25519") },
			{ "publicKey", cbor::Value::FromText(Base64Encode(*key)) },
			{ "value", cbor::Value::FromText(Base64Encode(*signature)) },
		}) });
	}
	const auto manifestText = json::Canonical(signedDocument);
	auto archiveFiles = std::vector<std::pair<std::string, cbor::Bytes>>();
	archiveFiles.emplace_back(
		"manifest.json",
		cbor::Bytes(manifestText.begin(), manifestText.end()));
	for (auto &file : files) {
		archiveFiles.push_back(std::move(file));
	}
	const auto archive = WriteZip(archiveFiles);
	const auto checked = OpenPackage(archive);
	if (!checked.ok()) {
		std::fprintf(stderr, "the package would not be accepted:\n");
		for (const auto &line : checked.error().details) {
			std::fprintf(stderr, "  %s\n", line.c_str());
		}
		return 2;
	}
	auto stream = std::ofstream(output, std::ios::binary | std::ios::trunc);
	if (!stream.write(reinterpret_cast<const char *>(archive.data()), std::streamsize(archive.size()))) {
		std::fprintf(stderr, "cannot write %s\n", output.c_str());
		return 1;
	}
	std::printf("wrote %s (%zu bytes)\n", output.c_str(), archive.size());
	return Describe(checked.value());
}

int SignRuntime(const std::string &path, const std::string &keyPath, const std::string &output) {
	const auto bytes = ReadFile(path);
	const auto seed = ReadSeed(keyPath);
	if (!bytes || !seed) {
		std::fprintf(stderr, "cannot read the index or the key\n");
		return 1;
	}
	auto document = json::Parse(std::string(bytes->begin(), bytes->end()));
	if (!document.ok() || !document.value().asMap()) {
		std::fprintf(stderr, "the index is not a JSON object\n");
		return 1;
	}
	auto map = cbor::Map();
	for (auto &entry : *document.value().asMap()) {
		if (entry.key != "signature") {
			map.push_back(std::move(entry));
		}
	}
	auto signedDocument = cbor::Value::FromMap(std::move(map));
	const auto text = SignedText(signedDocument);
	const auto signature = Ed25519Sign(*seed, {
		reinterpret_cast<const uint8_t *>(text.data()), text.size() });
	if (!signature) {
		std::fprintf(stderr, "cannot sign\n");
		return 1;
	}
	signedDocument.asMap()->push_back({ "signature", cbor::Value::FromMap({
		{ "alg", cbor::Value::FromText("ed25519") },
		{ "value", cbor::Value::FromText(Base64Encode(*signature)) },
	}) });
	const auto key = Ed25519PublicKey(*seed);
	if (!WriteFile(output, json::Canonical(signedDocument))) {
		std::fprintf(stderr, "cannot write %s\n", output.c_str());
		return 1;
	}
	std::printf("wrote %s, public key %s\n", output.c_str(), Base64Encode(*key).c_str());
	return 0;
}

int PackRuntime(const fs::path &directory, const std::string &output) {
	auto files = std::vector<std::pair<std::string, cbor::Bytes>>();
	for (const auto &entry : fs::recursive_directory_iterator(directory)) {
		if (!entry.is_regular_file()) {
			continue;
		}
		auto content = ReadFile(entry.path());
		if (!content) {
			std::fprintf(stderr, "cannot read %s\n", entry.path().string().c_str());
			return 1;
		}
		files.emplace_back(fs::relative(entry.path(), directory).generic_string(), std::move(*content));
	}
	std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) {
		return a.first < b.first;
	});
	const auto archive = WriteZip(files);
	auto stream = std::ofstream(output, std::ios::binary | std::ios::trunc);
	if (!stream.write(reinterpret_cast<const char *>(archive.data()), std::streamsize(archive.size()))) {
		std::fprintf(stderr, "cannot write %s\n", output.c_str());
		return 1;
	}
	const auto hashOf = [&](const char *name) {
		for (const auto &[path, content] : files) {
			if (path == name) {
				return HashText(content);
			}
		}
		return std::string();
	};
	std::printf(
		"wrote %s: \"sha256\": \"%s\", \"size\": %zu, \"hostSha256\": \"%s\", \"pythonSha256\": \"%s\"\n",
		output.c_str(),
		HashText(archive).c_str(),
		archive.size(),
		hashOf("fgpl_plugin_host").c_str(),
		hashOf("python/python.wasm").c_str());
	return 0;
}

int HashFile(const std::string &path) {
	const auto bytes = ReadFile(path);
	if (!bytes) {
		std::fprintf(stderr, "cannot read %s\n", path.c_str());
		return 1;
	}
	std::printf("\"sha256\": \"%s\", \"size\": %zu\n", HashText(*bytes).c_str(), bytes->size());
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	const auto usage = [] {
		std::fprintf(stderr,
			"usage:\n"
			"  fgpl_tool keygen <name>\n"
			"  fgpl_tool pack <directory> <output.fgpl> [--key <name>.key]\n"
			"  fgpl_tool verify <package.fgpl>\n"
			"  fgpl_tool sign-runtime <index.json> <name>.key <output.json>\n"
			"  fgpl_tool hash <file>\n"
			"  fgpl_tool pack-runtime <directory> <output.zip>\n");
		return 64;
	};
	if (argc < 3) {
		return usage();
	}
	const auto command = std::string(argv[1]);
	if (command == "keygen") {
		return KeyGen(argv[2]);
	} else if (command == "pack-runtime" && argc == 4) {
		return PackRuntime(argv[2], argv[3]);
	} else if (command == "hash") {
		return HashFile(argv[2]);
	} else if (command == "sign-runtime" && argc == 5) {
		return SignRuntime(argv[2], argv[3], argv[4]);
	} else if (command == "verify") {
		return Verify(argv[2]);
	} else if (command == "pack" && argc >= 4) {
		auto key = std::string();
		if (argc == 6 && std::string(argv[4]) == "--key") {
			key = argv[5];
		} else if (argc != 4) {
			return usage();
		}
		return Pack(argv[2], argv[3], key);
	}
	return usage();
}
