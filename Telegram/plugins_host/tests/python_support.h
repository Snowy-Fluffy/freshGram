#pragma once

#include "host_client.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace pysupport {

namespace fs = std::filesystem;
using namespace fgpl;

[[nodiscard]] inline cbor::Bytes ReadFile(const fs::path &path) {
	auto stream = std::ifstream(path, std::ios::binary);
	return { std::istreambuf_iterator<char>(stream), {} };
}

[[nodiscard]] inline LoadRequest PythonRequest(
		const fs::path &plugin = FGPL_PYTHON_PLUGIN_DIR,
		const char *main = "main.py") {
	const auto root = fs::path(FGPL_CPYTHON_DIR);
	auto request = LoadRequest();
	request.wasm = ReadFile(root / "python.wasm");
	const auto library = root / "lib";
	for (const auto &entry : fs::recursive_directory_iterator(library)) {
		if (entry.is_regular_file()) {
			request.files.emplace_back(
				"runtime/lib/" + fs::relative(entry.path(), library).generic_string(),
				ReadFile(entry.path()));
		}
	}
		for (const auto &entry : fs::directory_iterator(plugin)) {
		if (entry.is_regular_file()) {
			request.files.emplace_back(
				"plugin/" + entry.path().filename().generic_string(),
				ReadFile(entry.path()));
		}
	}
	request.args = { "python", "-S", "-B", std::string("/plugin/") + main };
	request.env = {
		"PYTHONHOME=/runtime",
		"PYTHONPATH=/plugin",
		"PYTHONDONTWRITEBYTECODE=1",
		"PYTHONNOUSERSITE=1",
		"PYTHONUTF8=1",
		"PYTHONUNBUFFERED=1",
		"PYTHONHASHSEED=0",
	};
	request.limits.callTimeoutMs = 20000;
	request.limits.memoryBytes = uint64_t(256) << 20;
	return request;
}

[[nodiscard]] inline bool Installed() {
	return fs::exists(fs::path(FGPL_CPYTHON_DIR) / "python.wasm");
}

} // namespace pysupport
