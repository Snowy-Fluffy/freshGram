#include "check.h"
#include "fgpl/module_cache.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace fgpl;
namespace fs = std::filesystem;

[[nodiscard]] std::vector<uint8_t> Bytes(std::string_view text) {
	return { text.begin(), text.end() };
}

[[nodiscard]] std::string Hex(const Digest &digest) {
	constexpr auto digits = "0123456789abcdef";
	auto result = std::string();
	for (const auto byte : digest) {
		result.push_back(digits[byte >> 4]);
		result.push_back(digits[byte & 15]);
	}
	return result;
}

void TestSha256Vectors() {
	CHECK(Hex(Sha256(Bytes(""))) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(Hex(Sha256(Bytes("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK(Hex(Sha256(Bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")))
		== "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	CHECK(Hex(Sha256(std::vector<uint8_t>(1000000, 'a')))
		== "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	auto boundary = std::vector<uint8_t>(55, 'a');
	CHECK(Hex(Sha256(boundary)) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
	boundary.push_back('a');
	CHECK(Hex(Sha256(boundary)) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
}

void TestHmacVectors() {
	CHECK(Hex(HmacSha256(std::vector<uint8_t>(20, 0x0b), Bytes("Hi There")))
		== "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
	CHECK(Hex(HmacSha256(Bytes("Jefe"), Bytes("what do ya want for nothing?")))
		== "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
	CHECK(Hex(HmacSha256(
			std::vector<uint8_t>(131, 0xaa),
			Bytes("Test Using Larger Than Block-Size Key - Hash Key First")))
		== "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

void TestConstantTimeEqual() {
	CHECK(ConstantTimeEqual(Bytes("abc"), Bytes("abc")));
	CHECK(!ConstantTimeEqual(Bytes("abc"), Bytes("abd")));
	CHECK(!ConstantTimeEqual(Bytes("abc"), Bytes("abcd")));
	CHECK(ConstantTimeEqual({}, {}));
}

[[nodiscard]] fs::path TempDirectory(const char *name) {
	const auto path = fs::temp_directory_path() / name;
	fs::remove_all(path);
	return path;
}

[[nodiscard]] CacheKey Key(uint8_t seed, std::string fingerprint = "v1") {
	auto key = CacheKey();
	key.secret.fill(seed);
	key.fingerprint = std::move(fingerprint);
	return key;
}

void Flip(const fs::path &path, size_t offset) {
	auto stream = std::fstream(path, std::ios::in | std::ios::out | std::ios::binary);
	stream.seekg(std::streamoff(offset));
	char byte = 0;
	stream.get(byte);
	stream.seekp(std::streamoff(offset));
	stream.put(char(byte ^ 1));
}

void TestStoreAndFind() {
	const auto directory = TempDirectory("fgpl-cache-basic");
	const auto cache = ModuleCache(directory, Key(1));
	const auto wasm = Bytes("a module");
	const auto compiled = Bytes("compiled native code");
	CHECK(!cache.find(wasm));
	CHECK(cache.store(wasm, compiled));
	const auto found = cache.find(wasm);
	CHECK(found && *found == compiled);
	CHECK(!cache.find(Bytes("another module")));
	cache.erase(wasm);
	CHECK(!cache.find(wasm));
	fs::remove_all(directory);
}

void TestTamperingIsRejected() {
	const auto directory = TempDirectory("fgpl-cache-tamper");
	const auto cache = ModuleCache(directory, Key(2));
	const auto wasm = Bytes("a module");
	CHECK(cache.store(wasm, Bytes("0123456789abcdef0123456789abcdef")));
	const auto path = cache.pathFor(wasm);
	const auto size = fs::file_size(path);
	for (const auto offset : { size_t(0), size_t(9), size_t(20), size_t(60), size_t(size / 2), size_t(size - 40), size_t(size - 1) }) {
		Flip(path, offset);
		if (cache.find(wasm)) {
			std::fprintf(stderr, "flip at %zu was accepted\n", offset);
			CHECK(false);
		}
		Flip(path, offset);
		CHECK(cache.find(wasm).has_value());
	}
	{
		auto stream = std::ofstream(path, std::ios::binary | std::ios::app);
		stream.put('x');
	}
	CHECK(!cache.find(wasm));
	fs::resize_file(path, size - 5);
	CHECK(!cache.find(wasm));
	fs::resize_file(path, 10);
	CHECK(!cache.find(wasm));
	fs::resize_file(path, 0);
	CHECK(!cache.find(wasm));
	fs::remove_all(directory);
}

void TestKeysAndFingerprintsDoNotMix() {
	const auto directory = TempDirectory("fgpl-cache-keys");
	const auto wasm = Bytes("a module");
	CHECK(ModuleCache(directory, Key(3)).store(wasm, Bytes("code")));
	CHECK(!ModuleCache(directory, Key(4)).find(wasm));
	CHECK(!ModuleCache(directory, Key(3, "v2")).find(wasm));
	CHECK(ModuleCache(directory, Key(3)).find(wasm).has_value());

	const auto other = Bytes("a different module");
	fs::copy_file(
		ModuleCache(directory, Key(3)).pathFor(wasm),
		ModuleCache(directory, Key(3)).pathFor(other));
	CHECK(!ModuleCache(directory, Key(3)).find(other));
	fs::remove_all(directory);
}

void TestPrune() {
	const auto directory = TempDirectory("fgpl-cache-prune");
	const auto cache = ModuleCache(directory, Key(5));
	for (auto i = 0; i != 4; ++i) {
		const auto wasm = Bytes("module " + std::to_string(i));
		CHECK(cache.store(wasm, std::vector<uint8_t>(1000, uint8_t(i))));
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	cache.prune(2500);
	CHECK(!cache.find(Bytes("module 0")));
	CHECK(!cache.find(Bytes("module 1")));
	CHECK(cache.find(Bytes("module 2")).has_value());
	CHECK(cache.find(Bytes("module 3")).has_value());
	cache.prune(0);
	CHECK(!cache.find(Bytes("module 3")));
	fs::remove_all(directory);
}

} // namespace

int main() {
	TestSha256Vectors();
	TestHmacVectors();
	TestConstantTimeEqual();
	TestStoreAndFind();
	TestTamperingIsRejected();
	TestKeysAndFingerprintsDoNotMix();
	TestPrune();
	return Failures() ? 1 : 0;
}
