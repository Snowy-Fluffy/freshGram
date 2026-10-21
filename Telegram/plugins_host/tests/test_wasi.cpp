#include "check.h"
#include "runtime.h"

#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace fgpl;

class LogBridge final : public Bridge {
public:
	cbor::Value call(const cbor::Value &) override {
		return {};
	}
	void log(std::string_view line) override {
		const auto equals = line.find('=');
		const auto key = std::string(line.substr(0, equals));
		const auto value = (equals == std::string_view::npos)
			? std::string()
			: std::string(line.substr(equals + 1));
		values[key].push_back(value);
	}

	[[nodiscard]] std::string one(const std::string &key) const {
		const auto i = values.find(key);
		return (i == values.end() || i->second.size() != 1)
			? std::string("<missing:" + key + ">")
			: i->second.front();
	}

	std::map<std::string, std::vector<std::string>> values;

};

[[nodiscard]] std::vector<uint8_t> ReadFile(const char *path) {
	auto stream = std::ifstream(path, std::ios::binary);
	return { std::istreambuf_iterator<char>(stream), {} };
}

[[nodiscard]] std::vector<uint8_t> Bytes(std::string_view text) {
	return { text.begin(), text.end() };
}

[[nodiscard]] std::shared_ptr<const Vfs> Files() {
	auto builder = VfsBuilder();
	CHECK(builder.addFile("plugin/hello.txt", Bytes("hello from the vfs\n")) == Errc::Ok);
	CHECK(builder.addFile("plugin/sub/inner.txt", Bytes("inner")) == Errc::Ok);
	CHECK(builder.addFile("plugin/data/a.txt", Bytes("a")) == Errc::Ok);
	CHECK(builder.addFile("plugin/data/b.txt", Bytes("b")) == Errc::Ok);
	CHECK(builder.addFile("runtime/lib/os.py", Bytes("import sys")) == Errc::Ok);
	CHECK(builder.addFile("secret/key.bin", Bytes("do not read")) == Errc::Ok);
	return builder.finish();
}

void TestProbe() {
	const auto wasm = ReadFile(FGPL_PROBE_WASM);
	CHECK(!wasm.empty());
	auto runtime = Runtime();
	auto bridge = LogBridge();
	auto loaded = runtime.load(wasm, bridge, {}, Files());
	CHECK(loaded.ok());
	if (!loaded.ok()) {
		std::fprintf(stderr, "load: %s\n", loaded.error().detail.c_str());
		return;
	}
	auto &plugin = *loaded.value();
	const auto result = plugin.deliver(cbor::Value());
	CHECK(result.ok());
	if (!result.ok()) {
		std::fprintf(stderr, "deliver: %s\n", plugin.lastError().c_str());
		return;
	}

	const auto check = [&](const char *key, const char *expected) {
		const auto actual = bridge.one(key);
		if (actual != expected) {
			std::fprintf(
				stderr,
				"%s: expected '%s', got '%s'\n",
				key,
				expected,
				actual.c_str());
			CHECK(false);
		}
	};
	const auto preopens = bridge.values["preopen"];
	CHECK(preopens.size() == 2);
	CHECK(preopens.size() == 2 && preopens[0] == "/plugin");
	CHECK(preopens.size() == 2 && preopens[1] == "/runtime");
	check("plugin_fd", "3");
	check("runtime_fd", "4");

	check("open_hello", "0");
	check("read_error", "0");
	check("content", "hello from the vfs");
	check("filestat_error", "0");
	check("filestat_type", "4");
	check("filestat_size", "19");
	check("seek_error", "0");
	check("seek_position", "5");
	check("after_seek", " fro");
	check("tell", "9");
	check("pread_error", "0");
	check("pread", "ello");
	check("seek_negative", "28");
	check("seek_stdout", "70");
	check("fdstat_error", "0");
	check("fdstat_type", "4");
	check("fdstat_dir_type", "3");
	check("fdstat_stdout_type", "2");

	check("open_missing", "44");
	check("open_dotdot", "76");
	check("open_absolute", "76");
	check("open_deep_dotdot", "76");
	check("open_inner_dotdot", "0");
	check("open_dir_as_file", "54");
	check("open_other_mount", "0");

	check("open_write_rights", "69");
	check("open_create", "69");
	check("open_truncate", "69");
	check("unlink", "69");
	check("stat_path_error", "0");
	check("stat_path_size", "5");
	check("stat_path_missing", "44");

	check("open_dir", "0");
	check("readdir_error", "0");
	const auto entries = bridge.values["entry"];
	CHECK(entries.size() == 2);
	CHECK(entries.size() == 2 && entries[0] == "a.txt");
	CHECK(entries.size() == 2 && entries[1] == "b.txt");
	check("readdir_used", "58");
	check("readdir_tiny", "30");
	check("readdir_past_end", "0");
	check("open_file_with_dir_flag", "54");
	check("readdir_on_file", "54");
	check("read_dir", "31");

	check("close", "0");
	check("read_closed", "8");
	check("close_closed", "8");
	check("opened_until_error", "62");
	check("limit_error", "33");
	check("reopen", "0");
	CHECK(bridge.values.contains("secret") == false);
}

void TestWithoutFiles() {
	const auto wasm = ReadFile(FGPL_PROBE_WASM);
	auto runtime = Runtime();
	auto bridge = LogBridge();
	auto loaded = runtime.load(wasm, bridge);
	CHECK(loaded.ok());
	if (!loaded.ok()) {
		return;
	}
	CHECK(loaded.value()->deliver(cbor::Value()).ok());
	CHECK(bridge.values["preopen"].empty());
	CHECK(bridge.one("plugin_fd") == "-1");
}

} // namespace

int main() {
	TestProbe();
	TestWithoutFiles();
	return Failures() ? 1 : 0;
}
