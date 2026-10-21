#include "check.h"
#include "fgpl/signature.h"
#include "zip_builder.h"
#include "fgpl/zip_reader.h"

#include <string>
#include <vector>

namespace {

using namespace fgpl;
using zipbuild::Item;

[[nodiscard]] std::vector<uint8_t> Bytes(std::string_view text) {
	return { text.begin(), text.end() };
}

[[nodiscard]] std::string Noise(size_t size) {
	auto result = std::string();
	auto state = uint32_t(12345);
	for (auto i = size_t(0); i != size; ++i) {
		state = state * 1664525u + 1013904223u;
		result.push_back(char(state >> 24));
	}
	return result;
}

[[nodiscard]] Item File(
		std::string name,
		std::string_view content,
		bool deflate = true) {
	auto item = Item();
	item.name = std::move(name);
	item.data = Bytes(content);
	item.deflate = deflate;
	return item;
}

void CheckRejected(
		const char *what,
		const std::vector<uint8_t> &archive,
		ZipError expected,
		const ZipLimits &limits = {}) {
	const auto result = ReadZip(archive, limits);
	if (result.ok()) {
		std::fprintf(stderr, "%s: accepted\n", what);
		CHECK(false);
	} else if (result.error().error != expected) {
		std::fprintf(
			stderr,
			"%s: error %d (%s), expected %d\n",
			what,
			int(result.error().error),
			result.error().detail.c_str(),
			int(expected));
		CHECK(false);
	}
}

void TestReadsOrdinaryArchives() {
	auto items = std::vector<Item>{
		File("manifest.json", "{\"a\": 1}"),
		File("plugin.wasm", Noise(5000)),
		File("lib/util.py", "x = 1\n", false),
		File("locales/ru.json", "{}"),
		File("empty.txt", ""),
	};
	auto folder = Item();
	folder.name = "lib";
	folder.directory = true;
	items.insert(items.begin() + 2, folder);
	const auto result = ReadZip(zipbuild::Build(items));
	CHECK(result.ok());
	if (!result.ok()) {
		std::fprintf(stderr, "%s\n", result.error().detail.c_str());
		return;
	}
	CHECK(result.value().size() == 5);
	CHECK(result.value()[0].name == "manifest.json");
	CHECK(result.value()[1].data.size() == 5000);
	CHECK(result.value()[2].name == "lib/util.py");
	CHECK(result.value()[4].data.empty());

	auto streamed = File("a.txt", "streamed content");
	streamed.descriptor = true;
	const auto withDescriptor = ReadZip(zipbuild::Build({ streamed }));
	CHECK(withDescriptor.ok() && withDescriptor.value()[0].data == Bytes("streamed content"));
}

void TestNamesAreChecked() {
	const auto build = [](const std::string &name) {
		return zipbuild::Build({ File(name, "x") });
	};
	for (const auto &name : std::vector<std::string>{
		"../evil.txt", "a/../../evil.txt", "/etc/passwd", "a\\b.txt", "C:/x",
		"a//b.txt", "./a.txt", "a/./b.txt", "a/", ".", "..", "a/..", "con:x",
		"trailing.", "trailing ", " leading", "a/ b", "bad|name", "bad*name",
		"tab\tname", "caf\xC3\xA9.txt", std::string("nul\0name", 8),
	}) {
		const auto result = ReadZip(build(name));
		if (result.ok()) {
			std::fprintf(stderr, "accepted name: %s\n", name.c_str());
			CHECK(false);
		}
	}
	auto longName = std::string();
	for (auto i = 0; i != 20; ++i) {
		longName += "d/";
	}
	CheckRejected("deep", build(longName + "f"), ZipError::Unsafe);
	CheckRejected("long", build(std::string(300, 'a')), ZipError::Unsafe);
	CheckRejected(
		"duplicate",
		zipbuild::Build({ File("a.txt", "1"), File("a.txt", "2") }),
		ZipError::Unsafe);
	CheckRejected(
		"duplicate by case",
		zipbuild::Build({ File("a.txt", "1"), File("A.TXT", "2") }),
		ZipError::Unsafe);
	CheckRejected(
		"file and directory",
		zipbuild::Build({ File("a", "1"), File("a/b", "2") }),
		ZipError::Unsafe);
	auto directory = Item();
	directory.name = "a";
	directory.directory = true;
	CheckRejected(
		"file and directory entry",
		zipbuild::Build({ File("a", "1"), directory }),
		ZipError::Unsafe);
}

void TestDangerousEntryKinds() {
	auto link = File("link", "target");
	link.externalAttributes = 0xA1FF0000;
	CheckRejected("symlink", zipbuild::Build({ link }), ZipError::Unsafe);
	auto device = File("dev", "x");
	device.externalAttributes = 0x61A40000;
	CheckRejected("device", zipbuild::Build({ device }), ZipError::Unsafe);
	auto encrypted = File("secret", "x");
	encrypted.flags = 1;
	CheckRejected("encrypted", zipbuild::Build({ encrypted }), ZipError::Unsupported);
	auto msdos = File("dos.txt", "x");
	msdos.madeBy = 0x0014;
	msdos.externalAttributes = 0x20;
	CHECK(ReadZip(zipbuild::Build({ msdos })).ok());
}

void TestBombsAndLimits() {
	const auto big = std::string(size_t(50) << 20, '\0');
	CheckRejected(
		"ratio",
		zipbuild::Build({ File("bomb.bin", big) }),
		ZipError::Unsafe);
	auto limits = ZipLimits();
	limits.maxEntryBytes = 1000;
	CheckRejected(
		"entry size",
		zipbuild::Build({ File("a", std::string(2000, 'x')) }),
		ZipError::TooLarge,
		limits);
	limits = ZipLimits();
	limits.maxTotalBytes = 3000;
	CheckRejected(
		"total size",
		zipbuild::Build({
			File("a", std::string(2000, 'x'), false),
			File("b", std::string(2000, 'y'), false),
		}),
		ZipError::TooLarge,
		limits);
	limits = ZipLimits();
	limits.maxEntries = 2;
	CheckRejected(
		"entry count",
		zipbuild::Build({ File("a", "1"), File("b", "2"), File("c", "3") }),
		ZipError::TooLarge,
		limits);
	limits = ZipLimits();
	limits.maxArchiveBytes = 100;
	CheckRejected(
		"archive size",
		zipbuild::Build({ File("a", std::string(500, 'x'), false) }),
		ZipError::TooLarge,
		limits);
}

void TestStructureIsStrict() {
	auto good = zipbuild::Build({ File("a.txt", "hello world"), File("b.txt", "second") });
	CHECK(ReadZip(good).ok());
	CheckRejected("trailing bytes", zipbuild::Build({ File("a", "x") }, 3), ZipError::Format);
	CheckRejected("empty", {}, ZipError::Format);
	CheckRejected("short", std::vector<uint8_t>(10, 0), ZipError::Format);
	CheckRejected("garbage", std::vector<uint8_t>(500, 0x41), ZipError::Format);

	auto badCrc = File("a.txt", "hello");
	badCrc.crcDelta = 1;
	CheckRejected("crc", zipbuild::Build({ badCrc }), ZipError::Corrupt);
	auto badDescriptorCrc = File("a.txt", "hello");
	badDescriptorCrc.crcDelta = 1;
	badDescriptorCrc.descriptor = true;
	CheckRejected("descriptor crc", zipbuild::Build({ badDescriptorCrc }), ZipError::Corrupt);

	auto mismatch = File("name.txt", "hello");
	mismatch.breakLocalName = true;
	CheckRejected("local name", zipbuild::Build({ mismatch }), ZipError::Format);

	auto hole = good;
	hole.insert(hole.begin() + 4, 0);
	CheckRejected("shifted data", hole, ZipError::Format);

	auto overlapping = zipbuild::Build({ File("a.txt", "hello world"), File("b.txt", "second") });
	const auto second = size_t(overlapping.size() - 22 - 46 - 5);
	overlapping[second + 42] = 0;
	overlapping[second + 43] = 0;
	overlapping[second + 44] = 0;
	overlapping[second + 45] = 0;
	CheckRejected("overlap", overlapping, ZipError::Format);

	for (const auto cut : { size_t(1), size_t(5), size_t(30), size_t(60) }) {
		auto truncated = good;
		truncated.resize(truncated.size() - cut);
		if (ReadZip(truncated).ok()) {
			std::fprintf(stderr, "truncated by %zu was accepted\n", cut);
			CHECK(false);
		}
	}

	auto corruptData = zipbuild::Build({ File("a.txt", std::string(2000, 'a')) });
	corruptData[40] ^= 0xFF;
	CHECK(!ReadZip(corruptData).ok());

	auto comment = zipbuild::Build({ File("a", "x") });
	comment[comment.size() - 2] = 3;
	comment.insert(comment.end(), { 'a', 'b', 'c' });
	CheckRejected("comment", comment, ZipError::Format);
}

void TestEveryByteFlipIsSafe() {
	const auto good = zipbuild::Build({
		File("manifest.json", "{\"id\": \"x\"}"),
		File("lib/a.py", std::string(300, 'z')),
		File("b.txt", "stored", false),
	});
	for (auto i = size_t(0); i != good.size(); ++i) {
		auto copy = good;
		copy[i] ^= 0x5A;
		const auto result = ReadZip(copy);
		if (result.ok()) {
			for (const auto &file : result.value()) {
				CHECK(file.name.find("..") == std::string::npos);
				CHECK(!file.name.empty() && file.name.front() != '/');
			}
		}
	}
}

void TestSignatures() {
	auto seed = PrivateSeed();
	seed.fill(7);
	const auto key = Ed25519PublicKey(seed);
	CHECK(key.has_value());
	if (!key) {
		return;
	}
	const auto message = Bytes("a message to sign");
	const auto signature = Ed25519Sign(seed, message);
	CHECK(signature.has_value());
	if (!signature) {
		return;
	}
	CHECK(Ed25519Verify(*key, message, *signature));
	CHECK(!Ed25519Verify(*key, Bytes("another message"), *signature));
	auto broken = *signature;
	broken[10] ^= 1;
	CHECK(!Ed25519Verify(*key, message, broken));
	auto otherSeed = seed;
	otherSeed[0] = 8;
	CHECK(!Ed25519Verify(*Ed25519PublicKey(otherSeed), message, *signature));
	CHECK(Ed25519Verify(*key, {}, *Ed25519Sign(seed, {})));
	CHECK(Fingerprint(*key).size() == 39);
	CHECK(Fingerprint(*key) != Fingerprint(*Ed25519PublicKey(otherSeed)));
}

} // namespace

int main() {
	TestReadsOrdinaryArchives();
	TestNamesAreChecked();
	TestDangerousEntryKinds();
	TestBombsAndLimits();
	TestStructureIsStrict();
	TestEveryByteFlipIsSafe();
	TestSignatures();
	return Failures() ? 1 : 0;
}
