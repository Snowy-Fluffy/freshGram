#include "check.h"
#include "fgpl/file_storage.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace fgpl;
namespace fs = std::filesystem;

[[nodiscard]] fs::path TempDirectory(const char *name) {
	const auto path = fs::temp_directory_path() / name;
	fs::remove_all(path);
	return path;
}

[[nodiscard]] cbor::Bytes Value(std::string_view text) {
	return { text.begin(), text.end() };
}

void Damage(const fs::path &file) {
	auto stream = std::ofstream(file, std::ios::binary | std::ios::trunc);
	stream << "not a cbor document";
}

void TestUsedBytesStaysConsistent() {
	const auto directory = TempDirectory("fgpl-storage-used");
	auto storage = FileStorage(directory / "store.cbor");
	CHECK(storage.usedBytes() == 0);
	storage.set("a", Value("12345"));
	CHECK(storage.usedBytes() == 6);
	storage.set("a", Value("12"));
	CHECK(storage.usedBytes() == 3);
	storage.set("bb", Value("1"));
	CHECK(storage.usedBytes() == 6);
	CHECK(storage.remove("a"));
	CHECK(!storage.remove("a"));
	CHECK(storage.usedBytes() == 3);
	CHECK(storage.count() == 1);
	CHECK(storage.flush());
	auto reloaded = FileStorage(directory / "store.cbor");
	CHECK(reloaded.usedBytes() == 3);
	fs::remove_all(directory);
}

void TestFlushKeepsPreviousVersion() {
	const auto directory = TempDirectory("fgpl-storage-backup");
	const auto file = directory / "store.cbor";
	{
		auto storage = FileStorage(file);
		storage.set("k", Value("one"));
		CHECK(storage.flush());
		CHECK(!fs::exists(directory / "store.cbor.bak"));
		storage.set("k", Value("two"));
		CHECK(storage.flush());
	}
	CHECK(fs::exists(directory / "store.cbor.bak"));
	auto reloaded = FileStorage(file);
	CHECK(reloaded.healthy());
	CHECK(!reloaded.recovered());
	CHECK(*reloaded.get("k") == Value("two"));
	fs::remove_all(directory);
}

void TestDamagedFileFallsBackToBackup() {
	const auto directory = TempDirectory("fgpl-storage-recover");
	const auto file = directory / "store.cbor";
	{
		auto storage = FileStorage(file);
		storage.set("k", Value("one"));
		CHECK(storage.flush());
		storage.set("k", Value("two"));
		CHECK(storage.flush());
	}
	Damage(file);
	{
		auto storage = FileStorage(file);
		CHECK(!storage.healthy());
		CHECK(storage.recovered());
		CHECK(fs::exists(directory / "store.cbor.corrupt"));
		CHECK(storage.get("k").has_value());
		CHECK(*storage.get("k") == Value("one"));
		CHECK(storage.usedBytes() == 4);
		CHECK(storage.flush());
	}
	auto healed = FileStorage(file);
	CHECK(healed.healthy());
	CHECK(!healed.recovered());
	CHECK(*healed.get("k") == Value("one"));
	fs::remove_all(directory);
}

void TestMissingMainFileUsesBackup() {
	const auto directory = TempDirectory("fgpl-storage-missing");
	const auto file = directory / "store.cbor";
	{
		auto storage = FileStorage(file);
		storage.set("k", Value("one"));
		CHECK(storage.flush());
		storage.set("k", Value("two"));
		CHECK(storage.flush());
	}
	fs::remove(file);
	auto storage = FileStorage(file);
	CHECK(storage.recovered());
	CHECK(*storage.get("k") == Value("one"));
	fs::remove_all(directory);
}

void TestDamagedWithoutBackupStartsEmpty() {
	const auto directory = TempDirectory("fgpl-storage-empty");
	const auto file = directory / "store.cbor";
	fs::create_directories(directory);
	Damage(file);
	auto storage = FileStorage(file);
	CHECK(!storage.healthy());
	CHECK(!storage.recovered());
	CHECK(storage.count() == 0);
	CHECK(storage.usedBytes() == 0);
	fs::remove_all(directory);
}

void TestDestructorFlushes() {
	const auto directory = TempDirectory("fgpl-storage-destructor");
	const auto file = directory / "store.cbor";
	{
		auto storage = FileStorage(file);
		storage.set("k", Value("kept"));
	}
	auto reloaded = FileStorage(file);
	CHECK(*reloaded.get("k") == Value("kept"));
	fs::remove_all(directory);
}

} // namespace

void TestTrySetKeepsTheLimits() {
	const auto directory = TempDirectory("fgpl-storage-try");
	auto storage = FileStorage(directory / "store.cbor");
	using Result = Storage::SetResult;
	CHECK(storage.trySet("a", Value("12345"), 20, 2) == Result::Ok);
	CHECK(storage.trySet("b", Value("12345678901234"), 20, 2) == Result::OverQuota);
	CHECK(storage.trySet("a", Value("12345678901234567890"), 20, 2) == Result::OverQuota);
	CHECK(storage.trySet("a", Value("1234567890123456"), 20, 2) == Result::Ok);
	CHECK(storage.usedBytes() == 17);
	CHECK(storage.trySet("b", Value(""), 20, 1) == Result::TooManyKeys);
	CHECK(storage.count() == 1);
}

int main() {
	TestTrySetKeepsTheLimits();
	TestUsedBytesStaysConsistent();
	TestFlushKeepsPreviousVersion();
	TestDamagedFileFallsBackToBackup();
	TestMissingMainFileUsesBackup();
	TestDamagedWithoutBackupStartsEmpty();
	TestDestructorFlushes();
	return Failures() ? 1 : 0;
}
