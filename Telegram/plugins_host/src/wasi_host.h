#pragma once

#include "vfs.h"

#include <wasmtime.hh>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace fgpl {

struct WasiConfig {
	std::shared_ptr<const Vfs> files;
	std::vector<std::string> mounts;
	size_t maxIoBytes = size_t(1) << 20;
	std::function<void(int fd, std::span<const uint8_t> bytes)> output;
};

class Wasi {
public:
	explicit Wasi(WasiConfig config);

	void link(
		wasmtime::Linker &linker,
		std::unordered_set<std::string> &defined);

	[[nodiscard]] const std::string &exitReason() const {
		return _exitReason;
	}

private:
	struct Fd {
		enum class Kind : uint8_t {
			Free,
			Stdio,
			Preopen,
			Node,
		};

		Kind kind = Kind::Free;
		VfsSession::Handle handle = 0;
		std::string name;
	};

	[[nodiscard]] const Fd *find(int32_t fd) const;
	[[nodiscard]] int32_t allocate(VfsSession::Handle handle);

	void linkClocks(
		wasmtime::Linker &linker,
		std::unordered_set<std::string> &defined);
	void linkFiles(
		wasmtime::Linker &linker,
		std::unordered_set<std::string> &defined);
	void linkDirectories(
		wasmtime::Linker &linker,
		std::unordered_set<std::string> &defined);
	void linkRefusals(
		wasmtime::Linker &linker,
		std::unordered_set<std::string> &defined);

	WasiConfig _config;
	std::unique_ptr<VfsSession> _session;
	std::vector<Fd> _fds;
	size_t _firstDynamic = 3;
	std::string _exitReason;

};

} // namespace fgpl
