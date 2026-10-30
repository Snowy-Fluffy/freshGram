#include "check.h"
#include "fgpl/base64.h"
#include "fgpl/json.h"
#include "fgpl/manifest.h"
#include "fgpl/os.h"
#include "fgpl/package.h"
#include "fgpl/runtime_bundle.h"
#include "fgpl/zip_reader.h"
#include "fgpl/zip_writer.h"

#include <filesystem>
#include <fstream>

namespace {

using namespace fgpl;
namespace fs = std::filesystem;

[[nodiscard]] cbor::Value Text(const std::string &text) {
	return cbor::Value::FromText(text);
}

[[nodiscard]] cbor::Value Int(int64_t number) {
	return cbor::Value::FromInt(number);
}

[[nodiscard]] PrivateSeed Seed(uint8_t fill) {
	auto result = PrivateSeed();
	result.fill(fill);
	return result;
}

[[nodiscard]] cbor::Bytes Bytes(const std::string &text) {
	return { text.begin(), text.end() };
}

[[nodiscard]] cbor::Bytes ValidArchive(const std::string &host = "host-binary") {
	return WriteZip({
		{ os::kHostFileName, Bytes(host) },
		{ "python/python.wasm", Bytes("wasm-python") },
		{ "python/lib/os.py", Bytes("import sys") },
		{ "sdk/freshgram/__init__.py", Bytes("") },
	});
}

[[nodiscard]] std::string FileHash(const cbor::Bytes &archive, const std::string &name) {
	const auto files = ReadZip(archive);
	if (files.ok()) {
		for (const auto &file : files.value()) {
			if (file.name == name) {
				return HashText(file.data);
			}
		}
	}
	return HashText(cbor::Bytes());
}

[[nodiscard]] cbor::Value IndexDocument(
		const cbor::Bytes &archive,
		int revision = 3,
		int minRevision = 1,
		int64_t post = 12,
		const std::string &hostHash = {}) {
	return cbor::Value::FromMap({
		{ "schema", Int(1) },
		{ "revision", Int(revision) },
		{ "minRevision", Int(minRevision) },
		{ "host", Int(1) },
		{ "wasmtime", Text("50.0.0") },
		{ "python", Text("3.14.0") },
		{ "revoked", cbor::Value::FromArray({}) },
		{ "bundles", cbor::Value::FromMap({
			{ "linux-x86_64", cbor::Value::FromMap({
				{ "post", Int(post) },
				{ "sha256", Text(HashText(archive)) },
				{ "size", Int(int64_t(archive.size())) },
				{ "hostSha256", Text(hostHash.empty()
					? FileHash(archive, os::kHostFileName)
					: hostHash) },
				{ "pythonSha256", Text(FileHash(archive, "python/python.wasm")) },
			}) },
		}) },
	});
}

[[nodiscard]] std::string Sign(cbor::Value document, uint8_t seedFill = 1) {
	const auto seed = Seed(seedFill);
	const auto text = SignedText(document);
	const auto signature = *Ed25519Sign(seed, {
		reinterpret_cast<const uint8_t*>(text.data()), text.size() });
	document.asMap()->push_back({ "signature", cbor::Value::FromMap({
		{ "alg", Text("ed25519") },
		{ "value", Text(Base64Encode(signature)) },
	}) });
	return json::Canonical(document);
}

[[nodiscard]] PublicKey Key(uint8_t fill = 1) {
	return *Ed25519PublicKey(Seed(fill));
}

struct Lab {
	Lab() {
		root = fs::temp_directory_path() / "fgpl_runtime_bundle";
		fs::remove_all(root);
	}
	~Lab() {
		fs::remove_all(root);
	}
	fs::path root;
};

void TestIndex() {
	const auto archive = ValidArchive();
	const auto text = Sign(IndexDocument(archive));
	const auto parsed = ParseRuntimeIndex(text, Key());
	CHECK(parsed.ok());
	if (parsed.ok()) {
		CHECK(parsed.value().revision == 3);
		CHECK(parsed.value().wasmtime == "50.0.0");
		CHECK(BundleFor(parsed.value(), "linux-x86_64") != nullptr);
		CHECK(BundleFor(parsed.value(), "linux-x86_64")->post == 12);
		CHECK(BundleFor(parsed.value(), "windows-x86_64") == nullptr);
		CHECK(Acceptable(parsed.value()));
	}

	CHECK(!ParseRuntimeIndex(text, Key(2)).ok());
	CHECK(!ParseRuntimeIndex(Sign(IndexDocument(archive), 2), Key()).ok());
	auto tampered = text;
	tampered.replace(tampered.find("50.0.0"), 6, "51.0.0");
	CHECK(!ParseRuntimeIndex(tampered, Key()).ok());
	CHECK(!ParseRuntimeIndex(json::Canonical(IndexDocument(archive)), Key()).ok());
	CHECK(!ParseRuntimeIndex("not json", Key()).ok());
	CHECK(!ParseRuntimeIndex(Sign(IndexDocument(archive, 3, 1, 0)), Key()).ok());
	CHECK(!ParseRuntimeIndex(Sign(IndexDocument(archive, 3, 1, int64_t(1) << 31)), Key()).ok());

	auto extra = IndexDocument(archive);
	extra.asMap()->push_back({ "surprise", Int(1) });
	CHECK(!ParseRuntimeIndex(Sign(extra), Key()).ok());

	const auto old = ParseRuntimeIndex(Sign(IndexDocument(archive, 2, 3)), Key());
	CHECK(old.ok() && !Acceptable(old.value()));
}

[[nodiscard]] RuntimeIndex Parsed(const cbor::Bytes &archive, int revision = 3, int minRevision = 1) {
	return ParseRuntimeIndex(Sign(IndexDocument(archive, revision, minRevision)), Key()).value();
}

void TestInstall() {
	auto lab = Lab();
	const auto archive = ValidArchive();
	const auto index = Parsed(archive);
	const auto &bundle = *BundleFor(index, "linux-x86_64");

	const auto installed = InstallRuntime(archive, index, bundle, lab.root);
	CHECK(installed.ok());
	CHECK(fs::exists(os::ResolveCurrent(lab.root) / "python" / "lib" / "os.py"));
#ifndef _WIN32
	CHECK(fs::is_symlink(lab.root / "current"));
	CHECK((fs::status(os::ResolveCurrent(lab.root) / os::kHostFileName).permissions()
		& fs::perms::all) == fs::perms::owner_all);
#endif // !_WIN32

	const auto read = ReadInstalledRuntime(lab.root);
	CHECK(read && read->revision == 3 && read->python == "3.14.0");
	CHECK(read && VerifyInstalledRuntime(*read));

	{
		auto stream = std::ofstream(os::ResolveCurrent(lab.root) / os::kHostFileName, std::ios::binary | std::ios::trunc);
		stream << "evil";
	}
	CHECK(read && !VerifyInstalledRuntime(*read));

	RemoveRuntime(lab.root);
	CHECK(!ReadInstalledRuntime(lab.root));
}

void TestTrustedRead() {
	auto lab = Lab();
	const auto archive = ValidArchive();
	const auto index = Parsed(archive);
	CHECK(InstallRuntime(archive, index, *BundleFor(index, "linux-x86_64"), lab.root).ok());

	const auto trusted = ReadTrustedRuntime(lab.root, Key(), "linux-x86_64");
	CHECK(trusted && trusted->hostHash == HashText(Bytes("host-binary")));
	CHECK(trusted && trusted->pythonHash == HashText(Bytes("wasm-python")));
	CHECK(!ReadTrustedRuntime(lab.root, Key(2), "linux-x86_64"));
	CHECK(!ReadTrustedRuntime(lab.root, Key(), "linux-aarch64"));

	const auto info = os::ResolveCurrent(lab.root) / "installed.json";
	auto text = std::string();
	{
		auto stream = std::ifstream(info);
		text.assign(std::istreambuf_iterator<char>(stream), {});
	}
	{
		auto forged = text;
		forged.replace(forged.find("\"hostHash\":\"") + 12, 10, "sha256-AAA");
		auto stream = std::ofstream(info, std::ios::trunc);
		stream << forged;
	}
	const auto forged = ReadTrustedRuntime(lab.root, Key(), "linux-x86_64");
	CHECK(forged && forged->hostHash == HashText(Bytes("host-binary")));
	{
		auto stream = std::ofstream(info, std::ios::trunc);
		stream << text;
	}

	fs::remove(os::ResolveCurrent(lab.root) / "index.json");
	CHECK(ReadInstalledRuntime(lab.root));
	CHECK(!ReadTrustedRuntime(lab.root, Key(), "linux-x86_64"));
}

void TestRefusals() {
	auto lab = Lab();
	const auto archive = ValidArchive();
	const auto index = Parsed(archive);
	const auto &bundle = *BundleFor(index, "linux-x86_64");

	auto wrong = ValidArchive("another host");
	CHECK(!InstallRuntime(wrong, index, bundle, lab.root).ok());
	auto cut = archive;
	cut.pop_back();
	CHECK(!InstallRuntime(cut, index, bundle, lab.root).ok());
	CHECK(!fs::exists(lab.root / "current"));

	const auto mismatched = ParseRuntimeIndex(
		Sign(IndexDocument(archive, 3, 1, 12, HashText(Bytes("other")))),
		Key()).value();
	CHECK(!InstallRuntime(archive, mismatched, *BundleFor(mismatched, "linux-x86_64"), lab.root).ok());
	auto unsignedIndex = index;
	unsignedIndex.text.clear();
	CHECK(!InstallRuntime(archive, unsignedIndex, bundle, lab.root).ok());

	const auto incomplete = WriteZip({ { os::kHostFileName, Bytes("x") } });
	const auto incompleteIndex = Parsed(incomplete);
	CHECK(!InstallRuntime(incomplete, incompleteIndex, *BundleFor(incompleteIndex, "linux-x86_64"), lab.root).ok());

	for (const auto &name : { "other", "python/../x", "/etc/passwd", "sdk" }) {
		const auto sneaky = WriteZip({
			{ os::kHostFileName, Bytes("x") },
			{ "python/python.wasm", Bytes("w") },
			{ "python/lib/a.py", Bytes("") },
			{ "sdk/freshgram/b.py", Bytes("") },
			{ name, Bytes("boo") },
		});
		const auto sneakyIndex = Parsed(sneaky);
		CHECK(!InstallRuntime(sneaky, sneakyIndex, *BundleFor(sneakyIndex, "linux-x86_64"), lab.root).ok());
	}
	CHECK(!fs::exists(lab.root / "current"));
}

void TestPolicyAndUpgrades() {
	auto lab = Lab();
	const auto first = ValidArchive("host one");
	const auto firstIndex = Parsed(first, 3);
	CHECK(InstallRuntime(first, firstIndex, *BundleFor(firstIndex, "linux-x86_64"), lab.root).ok());

	const auto second = ValidArchive("host two");
	const auto secondIndex = Parsed(second, 4);
	CHECK(InstallRuntime(second, secondIndex, *BundleFor(secondIndex, "linux-x86_64"), lab.root).ok());
	CHECK(ReadInstalledRuntime(lab.root)->revision == 4);
	CHECK(fs::exists(lab.root / "rev-3"));

	const auto third = ValidArchive("host three");
	const auto thirdIndex = Parsed(third, 5);
	CHECK(InstallRuntime(third, thirdIndex, *BundleFor(thirdIndex, "linux-x86_64"), lab.root).ok());
	CHECK(!fs::exists(lab.root / "rev-3"));
	CHECK(fs::exists(lab.root / "rev-4"));

	auto policy = Parsed(third, 6, 5);
	SaveRuntimePolicy(lab.root, policy);
	CHECK(ReadInstalledRuntime(lab.root)->revision == 5);
	policy = Parsed(third, 7, 6);
	SaveRuntimePolicy(lab.root, policy);
	CHECK(!ReadInstalledRuntime(lab.root));

	const auto again = Parsed(first, 3);
	CHECK(!InstallRuntime(first, again, *BundleFor(again, "linux-x86_64"), lab.root).ok());

	SaveRuntimePolicy(lab.root, Parsed(third, 8, 1));
	CHECK(!InstallRuntime(third, thirdIndex, *BundleFor(thirdIndex, "linux-x86_64"), lab.root).ok());
}

} // namespace

int main() {
	TestIndex();
	TestInstall();
	TestTrustedRead();
	TestRefusals();
	TestPolicyAndUpgrades();
	return Failures() ? 1 : 0;
}
