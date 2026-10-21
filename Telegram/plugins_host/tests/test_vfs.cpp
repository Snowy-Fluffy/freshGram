#include "check.h"
#include "vfs.h"

#include <array>
#include <cstring>
#include <random>
#include <string>

namespace {

using namespace fgpl;

[[nodiscard]] std::vector<uint8_t> Bytes(std::string_view text) {
	return { text.begin(), text.end() };
}

[[nodiscard]] std::shared_ptr<const Vfs> SampleTree(
		const VfsLimits &limits = {}) {
	auto builder = VfsBuilder(limits);
	CHECK(builder.addFile("plugin/main.py", Bytes("print('hi')")) == Errc::Ok);
	CHECK(builder.addFile("plugin/lib/util.py", Bytes("x = 1")) == Errc::Ok);
	CHECK(builder.addFile("plugin/lib/data.json", Bytes("{}")) == Errc::Ok);
	CHECK(builder.addFile("plugin/empty.txt", {}) == Errc::Ok);
	CHECK(builder.addDirectory("plugin/assets") == Errc::Ok);
	CHECK(builder.addFile("runtime/lib/os.py", Bytes("import sys")) == Errc::Ok);
	CHECK(builder.addFile("runtime/lib/zlib.py", Bytes("z")) == Errc::Ok);
	return builder.finish();
}

[[nodiscard]] std::string Contents(
		VfsSession &session,
		VfsSession::Handle handle) {
	auto result = std::string();
	auto buffer = std::array<uint8_t, 4>();
	while (true) {
		const auto count = session.read(handle, buffer);
		CHECK(count.ok());
		if (!count.ok() || count.value() == 0) {
			break;
		}
		result.append(buffer.begin(), buffer.begin() + count.value());
	}
	return result;
}

void TestNames() {
	CHECK(IsValidName("main.py", 128));
	CHECK(IsValidName("__pycache__", 128));
	CHECK(IsValidName(".hidden", 128));
	CHECK(IsValidName("a-b_c+d@e~f", 128));
	CHECK(!IsValidName("", 128));
	CHECK(!IsValidName(".", 128));
	CHECK(!IsValidName("..", 128));
	CHECK(!IsValidName("...", 128));
	CHECK(!IsValidName("a/b", 128));
	CHECK(!IsValidName("a\\b", 128));
	CHECK(!IsValidName("a:b", 128));
	CHECK(!IsValidName("a*b", 128));
	CHECK(!IsValidName("a|b", 128));
	CHECK(!IsValidName(" lead", 128));
	CHECK(!IsValidName("trail ", 128));
	CHECK(!IsValidName("trail.", 128));
	CHECK(!IsValidName(std::string("a\0b", 3), 128));
	CHECK(!IsValidName("tab\there", 128));
	CHECK(!IsValidName("del\x7f", 128));
	CHECK(!IsValidName("\xC3\xBC", 128));
	CHECK(!IsValidName(std::string(129, 'a'), 128));
	CHECK(IsValidName(std::string(128, 'a'), 128));
}

void TestParsePath() {
	const auto limits = VfsLimits();
	CHECK(ParsePath("", limits).error() == Errc::NotFound);
	CHECK(ParsePath("/abs", limits).error() == Errc::NotCapable);
	CHECK(ParsePath("..", limits).error() == Errc::NotCapable);
	CHECK(ParsePath("a/../..", limits).error() == Errc::NotCapable);
	CHECK(ParsePath("a/../../b", limits).error() == Errc::NotCapable);
	CHECK(ParsePath(std::string("a\0b", 3), limits).error()
		== Errc::InvalidArgument);
	CHECK(ParsePath(std::string(513, 'a'), limits).error()
		== Errc::NameTooLong);
	CHECK(ParsePath(std::string(129, 'a'), limits).error()
		== Errc::NameTooLong);

	const auto simple = ParsePath("a//./b/", limits);
	CHECK(simple.ok());
	if (simple.ok()) {
		CHECK(simple.value().parts.size() == 2);
		CHECK(simple.value().directoryOnly);
	}
	const auto dot = ParsePath(".", limits);
	CHECK(dot.ok() && dot.value().parts.empty());
	const auto back = ParsePath("a/b/../c", limits);
	CHECK(back.ok() && back.value().parts.size() == 4);

	auto deep = std::string();
	for (auto i = 0; i != 33; ++i) {
		deep += "d/";
	}
	CHECK(ParsePath(deep, limits).error() == Errc::NameTooLong);
	CHECK(ParsePath("d/d/d", limits).ok());
}

void TestBuilderRules() {
	auto builder = VfsBuilder();
	CHECK(builder.addFile("a.txt", Bytes("1")) == Errc::Ok);
	CHECK(builder.addFile("a.txt", Bytes("2")) == Errc::AlreadyExists);
	CHECK(builder.addFile("A.TXT", Bytes("2")) == Errc::AlreadyExists);
	CHECK(builder.addFile("a.txt/inner", Bytes("2")) == Errc::AlreadyExists);
	CHECK(builder.addFile("d/x", Bytes("1")) == Errc::Ok);
	CHECK(builder.addFile("D/y", Bytes("1")) == Errc::AlreadyExists);
	CHECK(builder.addFile("d", Bytes("1")) == Errc::AlreadyExists);
	CHECK(builder.addDirectory("d") == Errc::Ok);
	CHECK(builder.addDirectory("a.txt") == Errc::AlreadyExists);
	CHECK(builder.addFile("../x", Bytes("1")) == Errc::InvalidArgument);
	CHECK(builder.addFile("/x", Bytes("1")) == Errc::InvalidArgument);
	CHECK(builder.addFile("a//b", Bytes("1")) == Errc::InvalidArgument);
	CHECK(builder.addFile("a/", Bytes("1")) == Errc::InvalidArgument);
	CHECK(builder.addFile("", Bytes("1")) == Errc::InvalidArgument);
	CHECK(builder.addFile("a\\b", Bytes("1")) == Errc::InvalidArgument);
	CHECK(builder.addDirectory("") == Errc::InvalidArgument);
}

void TestBuilderLimits() {
	auto limits = VfsLimits();
	limits.maxFiles = 2;
	auto builder = VfsBuilder(limits);
	CHECK(builder.addFile("a", Bytes("1")) == Errc::Ok);
	CHECK(builder.addFile("b", Bytes("1")) == Errc::Ok);
	CHECK(builder.addFile("c", Bytes("1")) == Errc::LimitExceeded);

	limits = VfsLimits();
	limits.maxTotalBytes = 10;
	builder = VfsBuilder(limits);
	CHECK(builder.addFile("a", Bytes("123456")) == Errc::Ok);
	CHECK(builder.addFile("b", Bytes("12345")) == Errc::LimitExceeded);
	CHECK(builder.addFile("c", Bytes("1234")) == Errc::Ok);
	CHECK(builder.finish()->totalBytes() == 10);

	limits = VfsLimits();
	limits.maxDirectories = 3;
	builder = VfsBuilder(limits);
	CHECK(builder.addDirectory("a") == Errc::Ok);
	CHECK(builder.addDirectory("b") == Errc::Ok);
	CHECK(builder.addDirectory("c") == Errc::LimitExceeded);
	CHECK(builder.addFile("a/f", Bytes("1")) == Errc::Ok);

	limits = VfsLimits();
	limits.maxDepth = 2;
	builder = VfsBuilder(limits);
	CHECK(builder.addFile("a/b", Bytes("1")) == Errc::Ok);
	CHECK(builder.addFile("a/b/c", Bytes("1")) == Errc::InvalidArgument);
}

void TestResolve() {
	const auto vfs = SampleTree();
	CHECK(vfs->fileCount() == 6);
	const auto root = Vfs::kRoot;

	const auto main = vfs->resolve(root, "plugin/main.py");
	CHECK(main.ok() && !vfs->isDirectory(main.value()));
	CHECK(vfs->size(main.value()) == 11);

	const auto plugin = vfs->resolve(root, "plugin");
	CHECK(plugin.ok() && vfs->isDirectory(plugin.value()));
	CHECK(vfs->resolve(root, "plugin/").ok());
	CHECK(vfs->resolve(root, "plugin/main.py/").error() == Errc::NotDirectory);
	CHECK(vfs->resolve(root, "plugin/main.py/x").error() == Errc::NotDirectory);
	CHECK(vfs->resolve(root, "nope").error() == Errc::NotFound);
	CHECK(vfs->resolve(root, "plugin/nope/x").error() == Errc::NotFound);
	CHECK(vfs->resolve(root, "Plugin").error() == Errc::NotFound);

	CHECK(vfs->resolve(plugin.value(), "lib/util.py").ok());
	CHECK(vfs->resolve(plugin.value(), "lib/../main.py").ok());
	CHECK(vfs->resolve(plugin.value(), "./lib/./util.py").ok());
	CHECK(vfs->resolve(plugin.value(), "..").error() == Errc::NotCapable);
	CHECK(vfs->resolve(plugin.value(), "../runtime/lib/os.py").error()
		== Errc::NotCapable);
	CHECK(vfs->resolve(plugin.value(), "lib/../../runtime").error()
		== Errc::NotCapable);
	CHECK(vfs->resolve(plugin.value(), "/runtime").error()
		== Errc::NotCapable);
	CHECK(vfs->resolve(plugin.value(), ".").value() == plugin.value());
	CHECK(vfs->resolve(main.value(), ".").error() == Errc::NotDirectory);
	CHECK(vfs->resolve(999, "x").error() == Errc::BadHandle);
	CHECK(vfs->resolve(plugin.value(), "main.py/..").error()
		== Errc::NotDirectory);
}

void TestChildrenAreSorted() {
	const auto vfs = SampleTree();
	const auto lib = vfs->resolve(Vfs::kRoot, "plugin/lib").value();
	const auto list = vfs->children(lib);
	CHECK(list.size() == 2);
	CHECK(vfs->name(list[0]) == "data.json");
	CHECK(vfs->name(list[1]) == "util.py");
	const auto top = vfs->children(Vfs::kRoot);
	CHECK(top.size() == 2);
	CHECK(vfs->name(top[0]) == "plugin");
	CHECK(vfs->name(top[1]) == "runtime");
}

void TestReadSeek() {
	const auto vfs = SampleTree();
	auto session = VfsSession(vfs);
	const auto root = session.openNode(Vfs::kRoot);
	CHECK(root.ok());
	const auto file = session.open(root.value(), "plugin/main.py");
	CHECK(file.ok());
	if (!file.ok()) {
		return;
	}
	CHECK(Contents(session, file.value()) == "print('hi')");
	CHECK(session.tell(file.value()).value() == 11);

	CHECK(session.seek(file.value(), 0, Whence::Set).value() == 0);
	auto four = std::array<uint8_t, 4>();
	CHECK(session.read(file.value(), four).value() == 4);
	CHECK(std::memcmp(four.data(), "prin", 4) == 0);
	CHECK(session.seek(file.value(), 2, Whence::Current).value() == 6);
	CHECK(session.seek(file.value(), -2, Whence::End).value() == 9);
	CHECK(session.read(file.value(), four).value() == 2);
	CHECK(session.read(file.value(), four).value() == 0);
	CHECK(session.seek(file.value(), 100, Whence::Set).value() == 100);
	CHECK(session.read(file.value(), four).value() == 0);
	CHECK(session.seek(file.value(), -1, Whence::Set).error()
		== Errc::InvalidArgument);
	CHECK(session.seek(file.value(), -12, Whence::End).error()
		== Errc::InvalidArgument);
	CHECK(session.tell(file.value()).value() == 100);
	CHECK(session.seek(
		file.value(),
		std::numeric_limits<int64_t>::min(),
		Whence::Current).error() == Errc::InvalidArgument);

	CHECK(session.readAt(file.value(), 6, four).value() == 4);
	CHECK(std::memcmp(four.data(), "'hi'", 4) == 0);
	CHECK(session.readAt(file.value(), 11, four).value() == 0);
	CHECK(session.readAt(
		file.value(),
		std::numeric_limits<uint64_t>::max(),
		four).value() == 0);
	CHECK(session.read(file.value(), std::span<uint8_t>()).value() == 0);

	const auto empty = session.open(root.value(), "plugin/empty.txt");
	CHECK(empty.ok() && session.read(empty.value(), four).value() == 0);
	CHECK(session.stat(empty.value()).value().size == 0);
}

void TestDirectoryOperations() {
	const auto vfs = SampleTree();
	auto session = VfsSession(vfs);
	const auto root = session.openNode(Vfs::kRoot).value();
	const auto dir = session.open(root, "plugin").value();
	auto four = std::array<uint8_t, 4>();
	CHECK(session.read(dir, four).error() == Errc::IsDirectory);
	CHECK(session.readAt(dir, 0, four).error() == Errc::IsDirectory);
	CHECK(session.seek(dir, 0, Whence::Set).error() == Errc::IsDirectory);
	CHECK(session.tell(dir).error() == Errc::IsDirectory);
	CHECK(session.stat(dir).value().directory);

	const auto file = session.open(dir, "main.py").value();
	CHECK(session.open(file, "x").error() == Errc::NotDirectory);
	CHECK(session.readDirectory(file, 0, 10).error() == Errc::NotDirectory);

	const auto all = session.readDirectory(dir, 0, 100);
	CHECK(all.ok() && all.value().size() == 4);
	if (all.ok() && all.value().size() == 4) {
		CHECK(all.value()[0].name == "assets" && all.value()[0].directory);
		CHECK(all.value()[1].name == "empty.txt");
		CHECK(all.value()[2].name == "lib" && all.value()[2].directory);
		CHECK(all.value()[3].name == "main.py" && !all.value()[3].directory);
		CHECK(all.value()[3].next == 4);
	}
	const auto page = session.readDirectory(dir, 2, 1);
	CHECK(page.ok() && page.value().size() == 1);
	CHECK(page.ok() && page.value()[0].name == "lib");
	CHECK(session.readDirectory(dir, 4, 10).value().empty());
	CHECK(session.readDirectory(dir, 99, 10).value().empty());
	CHECK(session.readDirectory(dir, 0, 0).value().empty());

	const auto path = session.statPath(root, "runtime/lib/os.py");
	CHECK(path.ok() && path.value().size == 10 && !path.value().directory);
	CHECK(session.statPath(dir, "../runtime").error() == Errc::NotCapable);
	CHECK(session.statPath(root, "plugin/assets").value().directory);
}

void TestHandles() {
	auto limits = VfsLimits();
	limits.maxOpenHandles = 3;
	const auto vfs = SampleTree(limits);
	auto session = VfsSession(vfs);
	const auto root = session.openNode(Vfs::kRoot).value();
	const auto a = session.open(root, "plugin/main.py");
	const auto b = session.open(root, "plugin/lib/util.py");
	CHECK(a.ok() && b.ok());
	CHECK(session.openCount() == 3);
	CHECK(session.open(root, "plugin/empty.txt").error() == Errc::TooManyOpen);
	CHECK(session.open(root, "missing").error() == Errc::NotFound);
	CHECK(session.openCount() == 3);

	CHECK(session.close(a.value()) == Errc::Ok);
	CHECK(session.openCount() == 2);
	CHECK(session.close(a.value()) == Errc::BadHandle);
	auto four = std::array<uint8_t, 4>();
	CHECK(session.read(a.value(), four).error() == Errc::BadHandle);
	CHECK(session.stat(a.value()).error() == Errc::BadHandle);
	CHECK(session.open(a.value(), "x").error() == Errc::BadHandle);

	const auto c = session.open(root, "plugin/empty.txt");
	CHECK(c.ok());
	CHECK(session.close(0) == Errc::BadHandle);
	CHECK(session.close(77) == Errc::BadHandle);
	CHECK(session.tell(77).error() == Errc::BadHandle);
	CHECK(session.seek(77, 0, Whence::Set).error() == Errc::BadHandle);
	CHECK(session.readDirectory(77, 0, 1).error() == Errc::BadHandle);
	CHECK(session.statPath(77, "x").error() == Errc::BadHandle);
}

void TestStaysInsideBase() {
	const auto vfs = SampleTree();
	const auto plugin = vfs->resolve(Vfs::kRoot, "plugin").value();
	const auto lib = vfs->resolve(Vfs::kRoot, "plugin/lib").value();
	const auto parts = std::array<std::string_view, 8>{
		"", ".", "..", "lib", "main.py", "assets", "runtime", "x"
	};
	auto random = std::mt19937(12345);
	for (auto round = 0; round != 200000; ++round) {
		auto path = std::string();
		const auto count = random() % 6;
		for (auto i = uint32_t(0); i != count; ++i) {
			path += parts[random() % parts.size()];
			path += '/';
		}
		if (random() % 2 && !path.empty()) {
			path.pop_back();
		}
		for (const auto base : { plugin, lib }) {
			const auto found = vfs->resolve(base, path);
			if (found.ok()) {
				CHECK(vfs->within(found.value(), base));
			}
		}
	}
}

} // namespace

int main() {
	TestNames();
	TestParsePath();
	TestBuilderRules();
	TestBuilderLimits();
	TestResolve();
	TestChildrenAreSorted();
	TestReadSeek();
	TestDirectoryOperations();
	TestHandles();
	TestStaysInsideBase();
	return Failures() ? 1 : 0;
}
