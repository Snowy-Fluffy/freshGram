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
	const auto command = std::string(FGPL_TOOL) + " " + arguments
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

} // namespace

int main() {
	const auto root = fs::temp_directory_path() / "fgpl-tool-test";
	fs::remove_all(root);
	fs::create_directories(root);
	const auto log = root / "out.txt";

	CHECK(Run("keygen " + (root / "dev").string(), log) == 0);
	CHECK(Read(log).find("fingerprint:") != std::string::npos);
	CHECK(fs::exists(root / "dev.key"));

	const auto source = root / "plugin";
	auto document = ValidDocument();
	Remove(document, "files");
	Write(source / "manifest.json", Bytes(json::Canonical(document)));
	Write(source / "plugin.wasm", ReactorWasm());
	Write(source / "docs" / "README.md", Bytes("# Demo\n"));

	const auto output = root / "demo.fgpl";
	CHECK(Run("pack " + source.string() + " " + output.string() + " --key " + (root / "dev.key").string(), log) == 0);
	const auto packed = Read(log);
	CHECK(packed.find("signature: valid") != std::string::npos);
	CHECK(packed.find("permission: messages.read (required)") != std::string::npos);
	CHECK(packed.find("risk:") != std::string::npos);

	CHECK(Run("verify " + output.string(), log) == 0);
	CHECK(Read(log).find("com.example.demo 1.2.0") != std::string::npos);

	const auto unsigned_ = root / "unsigned.fgpl";
	CHECK(Run("pack " + source.string() + " " + unsigned_.string(), log) == 0);
	CHECK(Read(log).find("signature: none") != std::string::npos);

	auto bytes = Read(output);
	bytes[bytes.size() / 2] ^= 0x40;
	Write(root / "broken.fgpl", std::vector<uint8_t>(bytes.begin(), bytes.end()));
	CHECK(Run("verify " + (root / "broken.fgpl").string(), log) == 2);

	Write(source / "helper.so", Bytes("x"));
	CHECK(Run("pack " + source.string() + " " + (root / "bad.fgpl").string(), log) == 2);
	CHECK(Read(log).find("helper.so") != std::string::npos);
	CHECK(!fs::exists(root / "bad.fgpl"));
	fs::remove(source / "helper.so");

	CHECK(Run("verify " + (root / "missing.fgpl").string(), log) == 1);
	CHECK(Run("", log) == 64);
	CHECK(Run("pack " + (root / "nothing").string() + " " + output.string(), log) == 1);

	fs::remove_all(root);
	return Failures() ? 1 : 0;
}
