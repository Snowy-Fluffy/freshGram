#include "check.h"
#include "fgpl/folder.h"
#include "fgpl/os.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif // !_WIN32

namespace {

using namespace fgpl;
namespace fs = std::filesystem;

[[nodiscard]] std::vector<uint8_t> Bytes(std::string_view text) {
	return { text.begin(), text.end() };
}

struct Sandbox {
	Sandbox() {
		auto random = std::array<uint8_t, 6>();
		os::RandomBytes(random);
		auto name = std::string("fgpl_folder_");
		for (const auto byte : random) {
			name += "0123456789abcdef"[byte & 15];
		}
		base = fs::temp_directory_path() / name;
		fs::create_directories(base / "root" / "sub");
		fs::create_directories(base / "outside");
		root = os::PathToUtf8(fs::canonical(base / "root"));
		outside = os::PathToUtf8(fs::canonical(base / "outside"));
		Put(base / "outside" / "secret.txt", "secret");
		Put(base / "root" / "a.txt", "hello");
		Put(base / "root" / "sub" / "b.txt", "inner");
	}
	~Sandbox() {
		auto error = std::error_code();
		fs::remove_all(base, error);
	}

	static void Put(const fs::path &file, const std::string &text) {
		auto stream = std::ofstream(file, std::ios::binary);
		stream << text;
	}

	fs::path base;
	std::string root;
	std::string outside;
};

[[nodiscard]] std::string ReadText(
		const std::string &root,
		std::string_view path) {
	const auto result = folder::Read(root, path, 0, 1 << 20);
	CHECK(result.ok());
	return result.ok()
		? std::string(result.value().begin(), result.value().end())
		: std::string();
}

void TestSplitPath() {
	CHECK(folder::SplitPath("a/b/c.txt", false).ok());
	CHECK(folder::SplitPath("a//b/./c", false).value().size() == 3);
	CHECK(folder::SplitPath("", true).ok());
	CHECK(!folder::SplitPath("", false).ok());
	CHECK(!folder::SplitPath("/etc/passwd", false).ok());
	CHECK(!folder::SplitPath("../x", false).ok());
	CHECK(!folder::SplitPath("a/../x", false).ok());
	CHECK(!folder::SplitPath("a\\b", false).ok());
	CHECK(!folder::SplitPath(std::string("a\0b", 3), false).ok());
	CHECK(!folder::SplitPath("a\nb", false).ok());
	CHECK(!folder::SplitPath(std::string(300, 'x'), false).ok());
	CHECK(folder::SplitPath("\xD0\xBF\xD1\x80/\xE2\x82\xAC", false).ok());
	CHECK(!folder::SplitPath("\xC0\xAF", false).ok());
	CHECK(!folder::SplitPath("\xED\xA0\x80", false).ok());
	CHECK(!folder::SplitPath("\xE2\x82", false).ok());
	auto deep = std::string();
	for (auto i = 0; i != 40; ++i) {
		deep += "d/";
	}
	CHECK(!folder::SplitPath(deep, false).ok());
}

void TestReadAndList() {
	const auto sandbox = Sandbox();
	CHECK(ReadText(sandbox.root, "a.txt") == "hello");
	CHECK(ReadText(sandbox.root, "sub/b.txt") == "inner");
	const auto part = folder::Read(sandbox.root, "a.txt", 1, 3);
	CHECK(part.ok() && std::string(part.value().begin(), part.value().end()) == "ell");
	CHECK(folder::Read(sandbox.root, "a.txt", 100, 3).value().empty());
	CHECK(folder::Read(sandbox.root, "missing.txt", 0, 3).error() == folder::Fail::NotFound);
	CHECK(!folder::Read(sandbox.root, "sub", 0, 3).ok());
	const auto listing = folder::List(sandbox.root, "", 0, 100);
	CHECK(listing.ok() && listing.value().entries.size() == 2);
	CHECK(listing.value().entries[0].name == "a.txt");
	CHECK(listing.value().entries[0].size == 5);
	CHECK(listing.value().entries[1].directory);
	const auto page = folder::List(sandbox.root, "", 0, 1);
	CHECK(page.value().entries.size() == 1 && page.value().more);
	const auto rest = folder::List(sandbox.root, "", 1, 1);
	CHECK(rest.value().entries.size() == 1 && !rest.value().more);
	CHECK(folder::Stat(sandbox.root, "sub/b.txt").value().size == 5);
	CHECK(folder::Stat(sandbox.root, "sub").value().directory);
}

void TestEscapesAreRefused() {
	const auto sandbox = Sandbox();
	CHECK(!folder::Read(sandbox.root, "../outside/secret.txt", 0, 100).ok());
	CHECK(!folder::Read(sandbox.root, "sub/../../outside/secret.txt", 0, 100).ok());
	CHECK(!folder::Read(sandbox.root, sandbox.outside + "/secret.txt", 0, 100).ok());
	CHECK(!folder::Write(
		sandbox.root,
		"../outside/new.txt",
		Bytes("x"),
		folder::WriteMode::Create).ok());
	CHECK(!fs::exists(sandbox.base / "outside" / "new.txt"));
	CHECK(!folder::Remove(sandbox.root, "../outside/secret.txt").ok());
	CHECK(fs::exists(sandbox.base / "outside" / "secret.txt"));
	CHECK(!folder::Rename(sandbox.root, "a.txt", "../outside/a.txt").ok());
	CHECK(!folder::List(sandbox.root, "..", 0, 10).ok());
}

void TestWrites() {
	const auto sandbox = Sandbox();
	using Mode = folder::WriteMode;
	CHECK(folder::Write(sandbox.root, "new.txt", Bytes("one"), Mode::Create).ok());
	CHECK(ReadText(sandbox.root, "new.txt") == "one");
	CHECK(folder::Write(sandbox.root, "new.txt", Bytes("two"), Mode::Create).error()
		== folder::Fail::Exists);
	CHECK(folder::Write(sandbox.root, "new.txt", Bytes("longer text"), Mode::Replace).ok());
	CHECK(ReadText(sandbox.root, "new.txt") == "longer text");
	CHECK(folder::Write(sandbox.root, "new.txt", Bytes("x"), Mode::Replace).ok());
	CHECK(ReadText(sandbox.root, "new.txt") == "x");
	CHECK(folder::Write(sandbox.root, "new.txt", Bytes("yz"), Mode::Append).ok());
	CHECK(ReadText(sandbox.root, "new.txt") == "xyz");
	CHECK(folder::Write(sandbox.root, "fresh.txt", Bytes("r"), Mode::Replace).ok());
	CHECK(folder::Write(sandbox.root, "nodir/x.txt", Bytes("r"), Mode::Create).error()
		== folder::Fail::NotFound);

	CHECK(folder::MakeDirectory(sandbox.root, "made").ok());
	CHECK(folder::MakeDirectory(sandbox.root, "made").ok());
	CHECK(folder::Write(sandbox.root, "made/in.txt", Bytes("i"), Mode::Create).ok());
	CHECK(folder::Remove(sandbox.root, "made").error() == folder::Fail::NotEmpty);
	CHECK(folder::Remove(sandbox.root, "made/in.txt").ok());
	CHECK(folder::Remove(sandbox.root, "made").ok());
	CHECK(folder::Remove(sandbox.root, "made").error() == folder::Fail::NotFound);

	CHECK(folder::Rename(sandbox.root, "new.txt", "moved.txt").ok());
	CHECK(ReadText(sandbox.root, "moved.txt") == "xyz");
	CHECK(folder::Rename(sandbox.root, "moved.txt", "a.txt").error()
		== folder::Fail::Exists);
	CHECK(ReadText(sandbox.root, "a.txt") == "hello");
	CHECK(!folder::Remove(sandbox.root, "").ok());
}

#ifndef _WIN32

void TestLinksAreNotFollowed() {
	const auto sandbox = Sandbox();
	using Mode = folder::WriteMode;
	fs::create_symlink(sandbox.base / "outside" / "secret.txt", sandbox.base / "root" / "link.txt");
	fs::create_directory_symlink(sandbox.base / "outside", sandbox.base / "root" / "dirlink");
	fs::create_symlink("a.txt", sandbox.base / "root" / "inner_link.txt");

	CHECK(!folder::Read(sandbox.root, "link.txt", 0, 100).ok());
	CHECK(!folder::Read(sandbox.root, "dirlink/secret.txt", 0, 100).ok());
	CHECK(!folder::Read(sandbox.root, "inner_link.txt", 0, 100).ok());
	CHECK(!folder::Write(sandbox.root, "link.txt", Bytes("pwn"), Mode::Replace).ok());
	CHECK(!folder::Write(sandbox.root, "dirlink/new.txt", Bytes("pwn"), Mode::Create).ok());
	CHECK(!fs::exists(sandbox.base / "outside" / "new.txt"));
	CHECK(!folder::List(sandbox.root, "dirlink", 0, 100).ok());
	CHECK(!folder::Stat(sandbox.root, "link.txt").ok());
	CHECK(!folder::MakeDirectory(sandbox.root, "dirlink/d").ok());
	CHECK(!folder::Rename(sandbox.root, "link.txt", "x.txt").ok());

	const auto listing = folder::List(sandbox.root, "", 0, 100);
	for (const auto &entry : listing.value().entries) {
		CHECK(entry.name != "link.txt" && entry.name != "dirlink" && entry.name != "inner_link.txt");
	}
	std::ifstream secret(sandbox.base / "outside" / "secret.txt");
	auto text = std::string();
	std::getline(secret, text);
	CHECK(text == "secret");
}

void TestSpecialFilesAndHardLinks() {
	const auto sandbox = Sandbox();
	using Mode = folder::WriteMode;
	const auto fifo = (sandbox.base / "root" / "pipe").string();
	CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
	CHECK(!folder::Read(sandbox.root, "pipe", 0, 10).ok());
	CHECK(!folder::Write(sandbox.root, "pipe", Bytes("x"), Mode::Replace).ok());
	CHECK(!folder::Stat(sandbox.root, "pipe").ok());
	const auto listing = folder::List(sandbox.root, "", 0, 100);
	for (const auto &entry : listing.value().entries) {
		CHECK(entry.name != "pipe");
	}

	fs::create_hard_link(sandbox.base / "outside" / "secret.txt", sandbox.base / "root" / "hard.txt");
	CHECK(!folder::Write(sandbox.root, "hard.txt", Bytes("pwn"), Mode::Replace).ok());
	CHECK(!folder::Write(sandbox.root, "hard.txt", Bytes("pwn"), Mode::Append).ok());
	std::ifstream secret(sandbox.base / "outside" / "secret.txt");
	auto text = std::string();
	std::getline(secret, text);
	CHECK(text == "secret");
}

void TestRootChecks() {
	const auto sandbox = Sandbox();
	CHECK(folder::CheckRoot(sandbox.root).ok());
	CHECK(folder::CheckRoot(sandbox.root + "/sub").ok());
	CHECK(!folder::CheckRoot("").ok());
	CHECK(!folder::CheckRoot("relative/dir").ok());
	CHECK(!folder::CheckRoot("/").ok());
	CHECK(!folder::CheckRoot("/etc").ok());
	CHECK(!folder::CheckRoot("/usr/lib").ok());
	CHECK(!folder::CheckRoot(sandbox.root + "/a.txt").ok());
	CHECK(!folder::CheckRoot(sandbox.root + "/missing").ok());
	if (const auto home = std::getenv("HOME"); home && *home) {
		CHECK(!folder::CheckRoot(home).ok());
	}
}

void TestRootSwapDoesNotEscape() {
	const auto sandbox = Sandbox();
	fs::rename(sandbox.base / "root", sandbox.base / "root_gone");
	fs::create_directory_symlink(sandbox.base / "outside", sandbox.base / "root");
	CHECK(!folder::Read(sandbox.root, "secret.txt", 0, 100).ok());
	CHECK(!folder::List(sandbox.root, "", 0, 100).ok());
}

#endif // !_WIN32

} // namespace

int main() {
	TestSplitPath();
	TestReadAndList();
	TestEscapesAreRefused();
	TestWrites();
#ifndef _WIN32
	TestLinksAreNotFollowed();
	TestSpecialFilesAndHardLinks();
	TestRootChecks();
	TestRootSwapDoesNotEscape();
#endif // !_WIN32
	return Failures() ? 1 : 0;
}
