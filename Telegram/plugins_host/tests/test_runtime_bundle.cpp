#include "check.h"
#include "fgpl/base64.h"
#include "fgpl/json.h"
#include "fgpl/manifest.h"
#include "fgpl/package.h"
#include "fgpl/runtime_bundle.h"
#include "fgpl/zip_writer.h"

#include <filesystem>
#include <fstream>
#include <sys/stat.h>

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
		{ "fgpl_plugin_host", Bytes(host) },
		{ "python/python.wasm", Bytes("wasm-python") },
		{ "python/lib/os.py", Bytes("import sys") },
		{ "sdk/freshgram/__init__.py", Bytes("") },
	});
}

[[nodiscard]] cbor::Value IndexDocument(
		const cbor::Bytes &archive,
		int revision = 3,
		int minRevision = 1,
		const std::string &url = "https://example.org/runtime.zip") {
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
				{ "url", Text(url) },
				{ "sha256", Text(HashText(archive)) },
				{ "size", Int(int64_t(archive.size())) },
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
	CHECK(!ParseRuntimeIndex(Sign(IndexDocument(archive, 3, 1, "http://example.org/a.zip")), Key()).ok());

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
	CHECK(fs::is_symlink(lab.root / "current"));
	CHECK(fs::exists(lab.root / "current" / "python" / "lib" / "os.py"));
	struct stat info{};
	CHECK(::stat((lab.root / "current" / "fgpl_plugin_host").c_str(), &info) == 0);
	CHECK((info.st_mode & 0777) == 0700);

	const auto read = ReadInstalledRuntime(lab.root);
	CHECK(read && read->revision == 3 && read->python == "3.14.0");
	CHECK(read && VerifyInstalledRuntime(*read));

	{
		auto stream = std::ofstream(lab.root / "current" / "fgpl_plugin_host", std::ios::binary | std::ios::trunc);
		stream << "evil";
	}
	CHECK(read && !VerifyInstalledRuntime(*read));

	RemoveRuntime(lab.root);
	CHECK(!ReadInstalledRuntime(lab.root));
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

	const auto incomplete = WriteZip({ { "fgpl_plugin_host", Bytes("x") } });
	const auto incompleteIndex = Parsed(incomplete);
	CHECK(!InstallRuntime(incomplete, incompleteIndex, *BundleFor(incompleteIndex, "linux-x86_64"), lab.root).ok());

	for (const auto &name : { "other", "python/../x", "/etc/passwd", "sdk" }) {
		const auto sneaky = WriteZip({
			{ "fgpl_plugin_host", Bytes("x") },
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
	TestRefusals();
	TestPolicyAndUpgrades();
	return Failures() ? 1 : 0;
}
