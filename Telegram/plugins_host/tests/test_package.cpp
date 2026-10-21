#include "abi.h"
#include "check.h"
#include "package_builder.h"
#include "wasi_host.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace fgpl;
using namespace pkg;
namespace fs = std::filesystem;

[[nodiscard]] std::map<std::string, std::vector<uint8_t>> Files() {
	return { { "plugin.wasm", ReactorWasm() }, { "README.md", Bytes("# Demo\n") } };
}

[[nodiscard]] std::string Noise(size_t size) {
	auto result = std::string();
	auto state = uint32_t(99);
	for (auto i = size_t(0); i != size; ++i) {
		state = state * 1664525u + 1013904223u;
		result.push_back(char('a' + ((state >> 24) % 26)));
	}
	return result;
}

[[nodiscard]] bool Mentions(const PackageFailure &failure, const std::string &fragment) {
	for (const auto &line : failure.details) {
		if (line.find(fragment) != std::string::npos) {
			return true;
		}
	}
	return false;
}

void ExpectFailure(
		const char *what,
		const Built &built,
		PackageError error,
		const std::string &fragment) {
	const auto result = OpenPackage(built.archive);
	if (result.ok()) {
		std::fprintf(stderr, "%s: accepted\n", what);
		CHECK(false);
		return;
	}
	if (result.error().error != error || !Mentions(result.error(), fragment)) {
		std::fprintf(
			stderr,
			"%s: error %d, expected %d mentioning '%s':\n",
			what,
			int(result.error().error),
			int(error),
			fragment.c_str());
		for (const auto &line : result.error().details) {
			std::fprintf(stderr, "    %s\n", line.c_str());
		}
		CHECK(false);
	}
}

void TestSignedPackage() {
	const auto seed = Seed(9);
	const auto built = Build(ValidDocument(), Files(), &seed);
	const auto result = OpenPackage(built.archive);
	if (!result.ok()) {
		for (const auto &line : result.error().details) {
			std::fprintf(stderr, "%s\n", line.c_str());
		}
		CHECK(false);
		return;
	}
	const auto &package = result.value();
	CHECK(package.signature == SignatureState::Valid);
	CHECK(package.publisher == Fingerprint(*Ed25519PublicKey(seed)));
	CHECK(package.manifest.id == "com.example.demo");
	CHECK(package.files.size() == 2);
	CHECK(package.files.at("README.md") == Bytes("# Demo\n"));
	CHECK(package.manifest.signature.has_value());

	const auto files = PluginFiles(package);
	CHECK(files.size() == 2);
	CHECK(files[0].first.rfind("plugin/", 0) == 0);

	const auto root = fs::temp_directory_path() / "fgpl-install-test";
	fs::remove_all(root);
	const auto first = InstallPackage(package, root);
	CHECK(first.ok());
	if (first.ok()) {
		CHECK(first.value() == root / "com.example.demo" / "1.2.0");
		CHECK(fs::exists(first.value() / "manifest.json"));
		CHECK(fs::exists(first.value() / "plugin.wasm"));
		auto stream = std::ifstream(first.value() / "manifest.json", std::ios::binary);
		auto text = std::string(std::istreambuf_iterator<char>(stream), {});
		CHECK(text == package.manifestText);
		CHECK(InstallPackage(package, root).ok());
		auto stale = 0;
		for (const auto &entry : fs::directory_iterator(root / "com.example.demo")) {
			stale += entry.path().filename().string().rfind(".staging", 0) == 0;
		}
		CHECK(stale == 0);
	}
	fs::remove_all(root);
}

void TestUnsignedAndOtherRuntimes() {
	const auto built = Build(ValidDocument(), Files());
	const auto result = OpenPackage(built.archive);
	CHECK(result.ok() && result.value().signature == SignatureState::Unsigned);
	CHECK(result.ok() && result.value().publisher.empty());

	auto python = ValidDocument();
	Set(python, "runtime.kind", Text("python"));
	Remove(python, "runtime.module");
	Set(python, "runtime.entry", Text("main.py"));
	Set(python, "contributes.events", Array({}));
	Remove(python, "permissions.required");
	const auto pythonPackage = Build(python, {
		{ "main.py", Bytes("#!/usr/bin/env python\nimport freshgram\nfreshgram.run()\n") },
	});
	CHECK(OpenPackage(pythonPackage.archive).ok());

	auto command = ValidDocument();
	const auto commandPackage = Build(command, { { "plugin.wasm", CommandWasm() } });
	CHECK(OpenPackage(commandPackage.archive).ok());
}

void TestTamperingIsDetected() {
	const auto seed = Seed(5);
	auto files = Files();
	auto wrongDocument = ValidDocument();
	auto wrongHashes = cbor::Map();
	for (const auto &file : files) {
		wrongHashes.push_back({
			file.first,
			Text(HashText(file.first == "README.md"
				? Bytes("something else")
				: file.second)),
		});
	}
	Set(wrongDocument, "files", cbor::Value::FromMap(std::move(wrongHashes)));
	const auto wrongHash = Build(wrongDocument, files, nullptr, false);
	ExpectFailure("wrong hash", wrongHash, PackageError::Files, "does not match its hash");

	auto signedBuild = Build(ValidDocument(), Files(), &seed);
	auto edited = signedBuild.document;
	Set(edited, "permissions.optional", cbor::Value::FromArray({
		cbor::Value::FromMap({ { "id", Text("net.any") }, { "reason", Text("surprise") } }),
	}));
	ExpectFailure(
		"edited after signing",
		Build(edited, Files(), nullptr, false),
		PackageError::Signature,
		"not valid");

	auto swapped = signedBuild.document;
	const auto other = Seed(6);
	Set(swapped, "signature.publicKey", Text(Base64Encode(*Ed25519PublicKey(other))));
	ExpectFailure(
		"swapped key",
		Build(swapped, Files(), nullptr, false),
		PackageError::Signature,
		"not valid");

	auto reordered = signedBuild.document;
	std::reverse(reordered.asMap()->begin(), reordered.asMap()->end());
	CHECK(OpenPackage(Build(reordered, Files(), nullptr, false).archive).ok());
}

void TestFileListMustMatch() {
	auto extra = Files();
	auto document = ValidDocument();
	auto built = Build(document, extra);
	extra["surprise.txt"] = Bytes("not listed");
	ExpectFailure(
		"unlisted file",
		Build(built.document, extra, nullptr, false),
		PackageError::Files,
		"not listed");

	auto ghostFiles = Files();
	auto ghostDoc = ValidDocument();
	auto hashes = cbor::Map();
	for (const auto &file : ghostFiles) {
		hashes.push_back({ file.first, Text(HashText(file.second)) });
	}
	hashes.push_back({ "ghost.txt", Text(HashText(Bytes("x"))) });
	Set(ghostDoc, "files", cbor::Value::FromMap(std::move(hashes)));
	ExpectFailure(
		"listed but missing",
		Build(ghostDoc, ghostFiles, nullptr, false),
		PackageError::Files,
		"listed but missing");

	auto selfDoc = ValidDocument();
	auto selfHashes = cbor::Map();
	for (const auto &file : Files()) {
		selfHashes.push_back({ file.first, Text(HashText(file.second)) });
	}
	selfHashes.push_back({ "manifest.json", Text(HashText(Bytes("x"))) });
	Set(selfDoc, "files", cbor::Value::FromMap(std::move(selfHashes)));
	ExpectFailure(
		"manifest in the list",
		Build(selfDoc, Files(), nullptr, false),
		PackageError::Files,
		"manifest.json");
}

void TestArchiveAndManifestFailures() {
	auto noManifest = zipbuild::Build({ [] {
		auto item = zipbuild::Item();
		item.name = "plugin.wasm";
		item.data = ReactorWasm();
		return item;
	}() });
	const auto missing = OpenPackage(noManifest);
	CHECK(!missing.ok() && missing.error().error == PackageError::MissingManifest);

	const auto garbage = OpenPackage(Bytes("this is not a zip file at all"));
	CHECK(!garbage.ok() && garbage.error().error == PackageError::Archive);

	auto notJson = zipbuild::Item();
	notJson.name = "manifest.json";
	notJson.data = Bytes("{ nope");
	const auto badJson = OpenPackage(zipbuild::Build({ notJson }));
	CHECK(!badJson.ok() && badJson.error().error == PackageError::Manifest);

	auto invalid = ValidDocument();
	Set(invalid, "id", Text("NO"));
	ExpectFailure("bad manifest", Build(invalid, Files()), PackageError::Manifest, "id");

	auto huge = ValidDocument();
	Set(huge, "x-padding", Text(Noise(300 * 1024)));
	ExpectFailure("huge manifest", Build(huge, Files()), PackageError::Manifest, "too large");

	auto bomb = zipbuild::Item();
	bomb.name = "bomb.bin";
	bomb.data = std::vector<uint8_t>(size_t(40) << 20, 0);
	const auto zipBomb = zipbuild::Build({ bomb });
	CHECK(!OpenPackage(zipBomb).ok());
}

void TestNativeCodeIsRefused() {
	const auto attempt = [&](const char *what, const std::string &name, std::vector<uint8_t> data) {
		auto files = Files();
		files[name] = std::move(data);
		ExpectFailure(what, Build(ValidDocument(), files), PackageError::Content, "'" + name + "'");
	};
	attempt("elf", "data.bin", { 0x7F, 'E', 'L', 'F', 2, 1, 1, 0 });
	attempt("pe", "tool.dat", { 'M', 'Z', 0x90, 0 });
	attempt("macho", "lib.dat", { 0xCF, 0xFA, 0xED, 0xFE, 7, 0, 0, 1 });
	attempt("class", "Thing.dat", { 0xCA, 0xFE, 0xBA, 0xBE, 0, 0, 0, 52 });
	attempt("dex", "classes.dat", Bytes("dex\n035\0"));
	attempt("so", "libhelper.so", Bytes("harmless text"));
	attempt("dll", "helper.dll", Bytes("harmless text"));
	attempt("exe", "run.EXE", Bytes("harmless text"));
	attempt("script", "run.sh", Bytes("echo hi"));
	attempt("shebang", "launcher.txt", Bytes("#!/bin/sh\necho hi\n"));
	attempt("nested zip", "inner.dat", { 'P', 'K', 3, 4, 0, 0 });
	attempt("pyd", "speedups.pyd", Bytes("x"));
	attempt("pyc", "main.pyc", Bytes("x"));
}

void TestRuntimeModuleIsChecked() {
	const auto wasm = [&](const char *what, std::vector<uint8_t> module, const std::string &fragment) {
		ExpectFailure(
			what,
			Build(ValidDocument(), { { "plugin.wasm", std::move(module) } }),
			PackageError::Runtime,
			fragment);
	};
	wasm("not wasm", Bytes("this is not wasm"), "not a WebAssembly");
	wasm("truncated", { 0, 'a', 's', 'm', 1, 0, 0, 0, 2, 0xFF }, "malformed");
	wasm(
		"forbidden import",
		wat::Wasm(wat::Module(R"((import "env" "system" (func)))", "(i32.const 0)")),
		"env::system");
	wasm(
		"unknown wasi import",
		wat::Wasm(wat::Module(R"((import "wasi_snapshot_preview1" "sock_connect" (func)))", "(i32.const 0)")),
		"sock_connect");
	wasm(
		"no exports",
		wat::Wasm("(module (memory (export \"memory\") 1))"),
		"must export");
	wasm(
		"no memory",
		wat::Wasm("(module (func (export \"_start\")))"),
		"must export");

	auto allowed = Build(ValidDocument(), { { "plugin.wasm", wat::Wasm(wat::Module(
		R"((import "wasi_snapshot_preview1" "fd_write"
			(func (param i32 i32 i32 i32) (result i32))))", "(i32.const 0)")) } });
	CHECK(OpenPackage(allowed.archive).ok());

	auto python = ValidDocument();
	Set(python, "runtime.kind", Text("python"));
	Remove(python, "runtime.module");
	Set(python, "runtime.entry", Text("main.py"));
	Set(python, "contributes.events", Array({}));
	Remove(python, "permissions.required");
	ExpectFailure(
		"binary entry",
		Build(python, { { "main.py", { 'a', 0, 'b' } } }),
		PackageError::Runtime,
		"UTF-8 text");
}

void TestIcon() {
	auto document = ValidDocument();
	Set(document, "icon", Text("icon.png"));
	auto files = Files();
	files["icon.png"] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3 };
	CHECK(OpenPackage(Build(document, files).archive).ok());
	files["icon.png"] = Bytes("GIF89a");
	ExpectFailure("not a png", Build(document, files), PackageError::Content, "PNG");
	files["icon.png"] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	const auto padding = Noise(300 * 1024);
	files["icon.png"].insert(files["icon.png"].end(), padding.begin(), padding.end());
	ExpectFailure("big icon", Build(document, files), PackageError::Content, "PNG");
}

void TestImportListMatchesTheImplementation() {
	auto engine = wasmtime::Engine();
	auto linker = wasmtime::Linker(engine);
	auto defined = std::unordered_set<std::string>();
	auto wasi = Wasi(WasiConfig());
	wasi.link(linker, defined);
	auto names = std::set<std::string>();
	for (const auto &key : defined) {
		const auto prefix = std::string("wasi_snapshot_preview1::");
		CHECK(key.rfind(prefix, 0) == 0);
		names.insert(key.substr(prefix.size()));
	}
	CHECK(names == AllowedWasiImports());
	CHECK(IsAllowedImport("freshgram", "call"));
	CHECK(!IsAllowedImport("freshgram", "other"));
	CHECK(!IsAllowedImport("env", "memory"));
}

} // namespace

int main() {
	TestSignedPackage();
	TestUnsignedAndOtherRuntimes();
	TestTamperingIsDetected();
	TestFileListMustMatch();
	TestArchiveAndManifestFailures();
	TestNativeCodeIsRefused();
	TestRuntimeModuleIsChecked();
	TestIcon();
	TestImportListMatchesTheImplementation();
	return Failures() ? 1 : 0;
}
