#pragma once

#include <set>
#include <string>
#include <optional>

namespace fgpl::dev {

struct RunOptions {
	// A plugin directory with manifest.json, or a .fgpl package.
	std::string target;
	// A file of REPL lines to run instead of reading the standard input.
	std::string script;
	// A runtime directory with python/ and sdk/, as in the downloaded bundle.
	std::string runtime;
	std::string host;
	std::string hostDirectory;
	// A directory that keeps the plugin's data between runs; empty for a temporary one.
	std::string state;
	std::string folder;
	std::string app = "7.5.10";
	std::string account = "dev";
	std::optional<std::set<std::string>> grant;
	std::set<std::string> deny;
	bool noSandbox = false;
};

// Runs the plugin against a small fake app and answers the process exit code.
[[nodiscard]] int Run(const RunOptions &options);

} // namespace fgpl::dev
