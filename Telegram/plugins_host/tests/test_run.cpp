#include "check.h"
#include "package_builder.h"

#include <sys/wait.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

using namespace pkg;
namespace fs = std::filesystem;

[[nodiscard]] int Run(const std::string &arguments, const fs::path &output) {
	const auto command = std::string(FGPL_TOOL) + " run " + arguments
		+ " > " + output.string() + " 2>&1";
	const auto status = std::system(command.c_str());
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

[[nodiscard]] std::string Read(const fs::path &path) {
	auto stream = std::ifstream(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(stream), {});
}

void Write(const fs::path &path, const std::vector<uint8_t> &data) {
	fs::create_directories(path.parent_path());
	auto stream = std::ofstream(path, std::ios::binary | std::ios::trunc);
	stream.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
}

[[nodiscard]] bool Has(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

} // namespace

int main() {
	const auto root = fs::temp_directory_path() / "fgpl-run-test";
	fs::remove_all(root);
	fs::create_directories(root);
	const auto log = root / "out.txt";

	const auto source = root / "plugin";
	auto document = ValidDocument();
	Remove(document, "files");
	Write(source / "manifest.json", Bytes(json::Canonical(document)));
	Write(source / "plugin.wasm", ReactorWasm());

	const auto script = root / "script.txt";
	Write(script, Bytes(
		"# comments and blank lines are skipped\n\n"
		"event message.new chat=1 text=\"hello world\"\n"
		"msg new 1 \"from the world\"\n"
		"event chat.opened chat=1\n"
		"info\n"));
	const auto common = source.string() + " --app 7.6.1 --script " + script.string();

	CHECK(Run(common, log) == 0);
	const auto output = Read(log);
	CHECK(Has(output, "[run] com.example.demo 1.2.0 (wasm), host sandboxed"));
	CHECK(Has(output, "granted: messages.read"));
	CHECK(Has(output, "queued=1 delivered=1"));
	CHECK(Has(output, "no plugin took it"));
	CHECK(Has(output, "granted messages.read (required)"));

	CHECK(Run(common + " --deny messages.read", log) == 0);
	CHECK(Has(Read(log), "granted: net.https storage.private"));

	Write(script, Bytes("bogus\nevent\n"));
	CHECK(Run(common, log) == 3);
	CHECK(Has(Read(log), "unknown command bogus"));
	CHECK(Has(Read(log), "2 command errors"));

	Write(script, Bytes("chat add 9 Friends group\nmsg add 9 text\nmsg new 8 text\n"));
	CHECK(Run(common, log) == 3);
	CHECK(Has(Read(log), "no such chat"));

	CHECK(Run(root.string() + "/missing", log) == 1);
	CHECK(Run(source.string() + " --app nonsense", log) == 1);
	CHECK(Run(source.string() + " --bogus", log) == 64);

	auto python = fs::path(FGPL_CPYTHON_DIR) / "python.wasm";
	if (fs::exists(python)) {
		Write(script, Bytes(
			"msg new 1 \"hi\"\n"
			"command hello chat=1\n"
			"storage\n"));
		CHECK(Run(std::string(FGPL_EXAMPLE_HELLO) + " --script " + script.string(), log) == 0);
		const auto hello = Read(log);
		CHECK(Has(hello, "[com.example.hello] message 1 in chat 1"));
		CHECK(Has(hello, "[app] send chat=1 \"Hello! I have seen 1 messages.\""));
		CHECK(Has(hello, "[run] count = 1"));

		const auto listener = root / "listener";
		fs::create_directories(listener);
		Write(listener / "manifest.json", Bytes(
			"{\"schema\":1,\"id\":\"com.example.listener\",\"name\":\"Listener\",\"version\":\"1.0.0\","
			"\"compat\":{\"freshgram\":\">=7.0.0 <8.0.0\",\"api\":6},"
			"\"runtime\":{\"kind\":\"python\",\"entry\":\"main.py\"},"
			"\"permissions\":{\"required\":["
			"{\"id\":\"transcribe.provide\",\"reason\":\"Hears.\"},"
			"{\"id\":\"media.read\",\"reason\":\"Needs the audio.\"}]},"
			"\"contributes\":{\"transcriber\":{\"name\":\"Listener\",\"maxSeconds\":60}}}"));
		Write(listener / "main.py", Bytes(
			"import freshgram\n\n\n"
			"@freshgram.on_transcribe\n"
			"def hear(request):\n"
			"    if request['video']:\n"
			"        raise Exception('no video')\n"
			"    return 'heard %d in %d' % (request['message'], request['chat'])\n\n\n"
			"freshgram.run()\n"));
		Write(script, Bytes(
			"chat add 1 Alice\n"
			"transcribe 1 100 seconds=10\n"
			"transcribe 1 100 seconds=61\n"
			"transcribe 1 100 video=true\n"));
		CHECK(Run(listener.string() + " --script " + script.string(), log) == 3);
		const auto heard = Read(log);
		CHECK(Has(heard, "[run] transcribe: \"heard 100 in 1\""));
		CHECK(Has(heard, "transcribe: too_long"));
		CHECK(Has(heard, "transcribe: no video"));
	}

	fs::remove_all(root);
	return Failures() ? 1 : 0;
}
