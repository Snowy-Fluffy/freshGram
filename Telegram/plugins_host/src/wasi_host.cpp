#include "wasi_host.h"

#include "guest.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <thread>

namespace fgpl {
namespace {

using namespace guest;

namespace code {

constexpr auto kSuccess = int32_t(0);
constexpr auto kBadf = int32_t(8);
constexpr auto kExist = int32_t(20);
constexpr auto kFault = int32_t(21);
constexpr auto kInval = int32_t(28);
constexpr auto kIsDir = int32_t(31);
constexpr auto kMFile = int32_t(33);
constexpr auto kNameTooLong = int32_t(37);
constexpr auto kNoEnt = int32_t(44);
constexpr auto kNoSys = int32_t(52);
constexpr auto kNotDir = int32_t(54);
constexpr auto kNotSup = int32_t(58);
constexpr auto kRoFs = int32_t(69);
constexpr auto kSPipe = int32_t(70);
constexpr auto kNotCapable = int32_t(76);

} // namespace code

namespace right {

constexpr auto kRead = uint64_t(1) << 1;
constexpr auto kSeek = uint64_t(1) << 2;
constexpr auto kSetFlags = uint64_t(1) << 3;
constexpr auto kTell = uint64_t(1) << 5;
constexpr auto kWrite = uint64_t(1) << 6;
constexpr auto kAdvise = uint64_t(1) << 7;
constexpr auto kPathOpen = uint64_t(1) << 13;
constexpr auto kReadDir = uint64_t(1) << 14;
constexpr auto kReadLink = uint64_t(1) << 15;
constexpr auto kPathStat = uint64_t(1) << 18;
constexpr auto kFdStat = uint64_t(1) << 21;

} // namespace right

namespace filetype {

constexpr auto kCharacterDevice = uint8_t(2);
constexpr auto kDirectory = uint8_t(3);
constexpr auto kRegularFile = uint8_t(4);

} // namespace filetype

constexpr auto kMaxPathBytes = uint32_t(1024);
constexpr auto kMaxVectors = int32_t(1024);
constexpr auto kClockGranularityNs = uint64_t(1000000);
constexpr auto kDirentHeader = size_t(24);

[[nodiscard]] int32_t ToErrno(Errc error) {
	switch (error) {
	case Errc::Ok: return code::kSuccess;
	case Errc::NotFound: return code::kNoEnt;
	case Errc::NotCapable: return code::kNotCapable;
	case Errc::NotDirectory: return code::kNotDir;
	case Errc::IsDirectory: return code::kIsDir;
	case Errc::InvalidArgument: return code::kInval;
	case Errc::NameTooLong: return code::kNameTooLong;
	case Errc::TooManyOpen: return code::kMFile;
	case Errc::BadHandle: return code::kBadf;
	case Errc::AlreadyExists: return code::kExist;
	case Errc::LimitExceeded: return code::kInval;
	}
	return code::kInval;
}

void WriteFilestat(
		wasmtime::Span<uint8_t> data,
		uint32_t at,
		uint8_t type,
		uint64_t id,
		uint64_t size) {
	std::memset(data.data() + at, 0, 64);
	PutU64(data, at + 8, id);
	PutU8(data, at + 16, type);
	PutU64(data, at + 24, 1);
	PutU64(data, at + 32, size);
}

[[nodiscard]] uint8_t FiletypeOf(bool directory) {
	return directory ? filetype::kDirectory : filetype::kRegularFile;
}

template <typename Read>
[[nodiscard]] int32_t ReadVectors(
		wasmtime::Span<uint8_t> data,
		int32_t vectors,
		int32_t count,
		int32_t donePointer,
		size_t maxTotal,
		Read &&read) {
	if (count < 0 || count > kMaxVectors) {
		return code::kInval;
	} else if (!InRange(data, uint32_t(vectors), uint64_t(count) * 8)
		|| !InRange(data, uint32_t(donePointer), 4)) {
		return code::kFault;
	}
	auto total = size_t(0);
	for (auto i = int32_t(0); i != count; ++i) {
		const auto at = uint32_t(vectors) + uint32_t(i) * 8;
		const auto pointer = GetU32(data, at);
		const auto length = size_t(GetU32(data, at + 4));
		if (!InRange(data, pointer, length)) {
			return code::kFault;
		}
		const auto wanted = std::min(length, maxTotal - total);
		if (wanted == 0) {
			break;
		}
		const auto done = read(std::span<uint8_t>(
			data.data() + pointer,
			wanted));
		if (!done.ok()) {
			if (total == 0) {
				return ToErrno(done.error());
			}
			break;
		}
		total += done.value();
		if (done.value() < wanted) {
			break;
		}
	}
	PutU32(data, uint32_t(donePointer), uint32_t(total));
	return code::kSuccess;
}

[[nodiscard]] int32_t ReadPath(
		wasmtime::Span<uint8_t> data,
		int32_t pointer,
		int32_t length,
		std::string_view &out) {
	if (length < 0 || uint32_t(length) > kMaxPathBytes) {
		return code::kNameTooLong;
	} else if (!InRange(data, uint32_t(pointer), uint32_t(length))) {
		return code::kFault;
	}
	out = std::string_view(
		reinterpret_cast<const char *>(data.data() + uint32_t(pointer)),
		uint32_t(length));
	return code::kSuccess;
}

} // namespace

Wasi::Wasi(WasiConfig config)
: _config(std::move(config)) {
	_fds.resize(3);
	for (auto &fd : _fds) {
		fd.kind = Fd::Kind::Stdio;
	}
	if (_config.files) {
		_session = std::make_unique<VfsSession>(_config.files);
		for (const auto &mount : _config.mounts) {
			const auto node = _config.files->resolve(Vfs::kRoot, mount);
			if (!node.ok() || !_config.files->isDirectory(node.value())) {
				continue;
			}
			const auto handle = _session->openNode(node.value());
			if (!handle.ok()) {
				continue;
			}
			auto &fd = _fds.emplace_back();
			fd.kind = Fd::Kind::Preopen;
			fd.handle = handle.value();
			fd.name = "/" + mount;
		}
	}
	_firstDynamic = _fds.size();
}

const Wasi::Fd *Wasi::find(int32_t fd) const {
	if (fd < 0 || size_t(fd) >= _fds.size()) {
		return nullptr;
	}
	const auto &slot = _fds[size_t(fd)];
	return (slot.kind == Fd::Kind::Free) ? nullptr : &slot;
}

int32_t Wasi::allocate(VfsSession::Handle handle) {
	auto index = _firstDynamic;
	while (index != _fds.size() && _fds[index].kind != Fd::Kind::Free) {
		++index;
	}
	if (index == _fds.size()) {
		_fds.emplace_back();
	}
	_fds[index].kind = Fd::Kind::Node;
	_fds[index].handle = handle;
	return int32_t(index);
}

void Wasi::link(
		wasmtime::Linker &linker,
		std::unordered_set<std::string> &defined) {
	linkClocks(linker, defined);
	linkFiles(linker, defined);
	linkDirectories(linker, defined);
	linkRefusals(linker, defined);
}

void Wasi::linkClocks(
		wasmtime::Linker &linker,
		std::unordered_set<std::string> &defined) {
	constexpr auto module = std::string_view("wasi_snapshot_preview1");
	const auto sizes = [](wasmtime::Caller caller, int32_t a, int32_t b)
	-> int32_t {
		const auto data = ViewOf(caller);
		if (!data
			|| !InRange(*data, uint32_t(a), 4)
			|| !InRange(*data, uint32_t(b), 4)) {
			return code::kFault;
		}
		PutU32(*data, uint32_t(a), 0);
		PutU32(*data, uint32_t(b), 0);
		return code::kSuccess;
	};
	Define(linker, defined, module, "args_sizes_get", sizes);
	Define(linker, defined, module, "environ_sizes_get", sizes);
	Define(linker, defined, module, "args_get", [](int32_t, int32_t) -> int32_t {
		return code::kSuccess;
	});
	Define(linker, defined, module, "environ_get", [](int32_t, int32_t) -> int32_t {
		return code::kSuccess;
	});
	Define(
		linker,
		defined,
		module,
		"clock_time_get",
		[](wasmtime::Caller caller, int32_t id, int64_t, int32_t timePointer)
		-> int32_t {
			const auto data = ViewOf(caller);
			if (!data || !InRange(*data, uint32_t(timePointer), 8)) {
				return code::kFault;
			} else if (id != 0 && id != 1) {
				return code::kInval;
			}
			const auto now = (id == 0)
				? std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::system_clock::now().time_since_epoch())
				: std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now().time_since_epoch());
			const auto value = uint64_t(now.count());
			PutU64(
				*data,
				uint32_t(timePointer),
				value - value % kClockGranularityNs);
			return code::kSuccess;
		});
	Define(
		linker,
		defined,
		module,
		"clock_res_get",
		[](wasmtime::Caller caller, int32_t id, int32_t pointer) -> int32_t {
			const auto data = ViewOf(caller);
			if (!data || !InRange(*data, uint32_t(pointer), 8)) {
				return code::kFault;
			} else if (id != 0 && id != 1) {
				return code::kInval;
			}
			PutU64(*data, uint32_t(pointer), kClockGranularityNs);
			return code::kSuccess;
		});
	Define(
		linker,
		defined,
		module,
		"random_get",
		[this](wasmtime::Caller caller, int32_t pointer, int32_t length)
		-> int32_t {
			const auto data = ViewOf(caller);
			if (!data) {
				return code::kFault;
			} else if (length < 0
				|| size_t(length) > _config.maxIoBytes) {
				return code::kInval;
			} else if (!InRange(*data, uint32_t(pointer), uint32_t(length))) {
				return code::kFault;
			}
			auto device = std::random_device();
			for (auto i = uint32_t(0); i < uint32_t(length);) {
				const auto word = device();
				for (auto k = 0; k != 4 && i < uint32_t(length); ++k, ++i) {
					data->data()[uint32_t(pointer) + i] = uint8_t(word >> (8 * k));
				}
			}
			return code::kSuccess;
		});
	Define(linker, defined, module, "sched_yield", []() -> int32_t {
		std::this_thread::yield();
		return code::kSuccess;
	});
	Define(
		linker,
		defined,
		module,
		"proc_exit",
		[this](int32_t exitCode)
		-> wasmtime::Result<std::monostate, wasmtime::Trap> {
			_exitReason = "the plugin called proc_exit("
				+ std::to_string(exitCode)
				+ ")";
			return wasmtime::Trap(_exitReason);
		});
}

void Wasi::linkFiles(
		wasmtime::Linker &linker,
		std::unordered_set<std::string> &defined) {
	constexpr auto module = std::string_view("wasi_snapshot_preview1");
	Define(
		linker,
		defined,
		module,
		"fd_write",
		[this](
			wasmtime::Caller caller,
			int32_t fd,
			int32_t vectors,
			int32_t count,
			int32_t writtenPointer) -> int32_t {
			const auto data = ViewOf(caller);
			if (!data) {
				return code::kFault;
			}
			const auto slot = find(fd);
			if (!slot || fd == 0 || slot->kind != Fd::Kind::Stdio) {
				return code::kBadf;
			} else if (count < 0 || count > kMaxVectors) {
				return code::kInval;
			} else if (!InRange(*data, uint32_t(vectors), uint64_t(count) * 8)
				|| !InRange(*data, uint32_t(writtenPointer), 4)) {
				return code::kFault;
			}
			auto total = uint64_t(0);
			for (auto i = int32_t(0); i != count; ++i) {
				const auto at = uint32_t(vectors) + uint32_t(i) * 8;
				const auto pointer = GetU32(*data, at);
				const auto length = GetU32(*data, at + 4);
				total += length;
				if (total > _config.maxIoBytes) {
					return code::kInval;
				} else if (!InRange(*data, pointer, length)) {
					return code::kFault;
				}
				if (_config.output) {
					_config.output(fd, std::span<const uint8_t>(
						data->data() + pointer,
						length));
				}
			}
			PutU32(*data, uint32_t(writtenPointer), uint32_t(total));
			return code::kSuccess;
		});
	Define(
		linker,
		defined,
		module,
		"fd_prestat_get",
		[this](wasmtime::Caller caller, int32_t fd, int32_t pointer) -> int32_t {
			const auto data = ViewOf(caller);
			const auto slot = find(fd);
			if (!data) {
				return code::kFault;
			} else if (!slot || slot->kind != Fd::Kind::Preopen) {
				return code::kBadf;
			} else if (!InRange(*data, uint32_t(pointer), 8)) {
				return code::kFault;
			}
			PutU8(*data, uint32_t(pointer), 0);
			PutU32(*data, uint32_t(pointer) + 4, uint32_t(slot->name.size()));
			return code::kSuccess;
		});
	Define(
		linker,
		defined,
		module,
		"fd_prestat_dir_name",
		[this](
			wasmtime::Caller caller,
			int32_t fd,
			int32_t pointer,
			int32_t length) -> int32_t {
			const auto data = ViewOf(caller);
			const auto slot = find(fd);
			if (!data) {
				return code::kFault;
			} else if (!slot || slot->kind != Fd::Kind::Preopen) {
				return code::kBadf;
			} else if (length < 0 || size_t(length) < slot->name.size()) {
				return code::kNameTooLong;
			} else if (!InRange(*data, uint32_t(pointer), slot->name.size())) {
				return code::kFault;
			}
			std::memcpy(
				data->data() + uint32_t(pointer),
				slot->name.data(),
				slot->name.size());
			return code::kSuccess;
		});
	Define(
		linker,
		defined,
		module,
		"fd_close",
		[this](int32_t fd) -> int32_t {
			const auto slot = find(fd);
			if (!slot) {
				return code::kBadf;
			} else if (slot->kind != Fd::Kind::Node) {
				return code::kSuccess;
			}
			const auto closed = _session->close(slot->handle);
			_fds[size_t(fd)] = Fd();
			return ToErrno(closed);
		});
	Define(
		linker,
		defined,
		module,
		"fd_fdstat_get",
		[this](wasmtime::Caller caller, int32_t fd, int32_t pointer) -> int32_t {
			const auto data = ViewOf(caller);
			const auto slot = find(fd);
			if (!data) {
				return code::kFault;
			} else if (!slot) {
				return code::kBadf;
			} else if (!InRange(*data, uint32_t(pointer), 24)) {
				return code::kFault;
			}
			std::memset(data->data() + uint32_t(pointer), 0, 24);
			auto type = filetype::kCharacterDevice;
			auto base = uint64_t(0);
			auto inheriting = uint64_t(0);
			if (slot->kind == Fd::Kind::Stdio) {
				base = (fd == 0) ? right::kRead : right::kWrite;
			} else {
				const auto stat = _session->stat(slot->handle);
				if (!stat.ok()) {
					return ToErrno(stat.error());
				}
				type = FiletypeOf(stat.value().directory);
				if (stat.value().directory) {
					base = right::kPathOpen
						| right::kReadDir
						| right::kReadLink
						| right::kPathStat
						| right::kFdStat
						| right::kSetFlags;
					inheriting = right::kRead
						| right::kSeek
						| right::kTell
						| right::kAdvise
						| right::kFdStat
						| right::kPathOpen
						| right::kReadDir
						| right::kReadLink
						| right::kPathStat
						| right::kSetFlags;
				} else {
					base = right::kRead
						| right::kSeek
						| right::kTell
						| right::kAdvise
						| right::kFdStat
						| right::kSetFlags;
				}
			}
			PutU8(*data, uint32_t(pointer), type);
			PutU64(*data, uint32_t(pointer) + 8, base);
			PutU64(*data, uint32_t(pointer) + 16, inheriting);
			return code::kSuccess;
		});
	Define(
		linker,
		defined,
		module,
		"fd_read",
		[this](
			wasmtime::Caller caller,
			int32_t fd,
			int32_t vectors,
			int32_t count,
			int32_t donePointer) -> int32_t {
			const auto data = ViewOf(caller);
			const auto slot = find(fd);
			if (!data) {
				return code::kFault;
			} else if (!slot) {
				return code::kBadf;
			} else if (slot->kind == Fd::Kind::Stdio) {
				if (fd != 0) {
					return code::kBadf;
				}
				return ReadVectors(
					*data,
					vectors,
					count,
					donePointer,
					_config.maxIoBytes,
					[](std::span<uint8_t>) -> Result<size_t, Errc> {
						return size_t(0);
					});
			}
			return ReadVectors(
				*data,
				vectors,
				count,
				donePointer,
				_config.maxIoBytes,
				[&](std::span<uint8_t> out) {
					return _session->read(slot->handle, out);
				});
		});
	Define(
		linker,
		defined,
		module,
		"fd_pread",
		[this](
			wasmtime::Caller caller,
			int32_t fd,
			int32_t vectors,
			int32_t count,
			int64_t offset,
			int32_t donePointer) -> int32_t {
			const auto data = ViewOf(caller);
			const auto slot = find(fd);
			if (!data) {
				return code::kFault;
			} else if (!slot || slot->kind == Fd::Kind::Stdio) {
				return code::kBadf;
			} else if (offset < 0) {
				return code::kInval;
			}
			auto position = uint64_t(offset);
			return ReadVectors(
				*data,
				vectors,
				count,
				donePointer,
				_config.maxIoBytes,
				[&](std::span<uint8_t> out) {
					const auto done = _session->readAt(
						slot->handle,
						position,
						out);
					if (done.ok()) {
						position += done.value();
					}
					return done;
				});
		});
	Define(
		linker,
		defined,
		module,
		"fd_seek",
		[this](
			wasmtime::Caller caller,
			int32_t fd,
			int64_t offset,
			int32_t whence,
			int32_t newPointer) -> int32_t {
			const auto data = ViewOf(caller);
			const auto slot = find(fd);
			if (!data) {
				return code::kFault;
			} else if (!slot) {
				return code::kBadf;
			} else if (slot->kind == Fd::Kind::Stdio) {
				return code::kSPipe;
			} else if (whence < 0 || whence > 2) {
				return code::kInval;
			} else if (!InRange(*data, uint32_t(newPointer), 8)) {
				return code::kFault;
			}
			const auto position = _session->seek(
				slot->handle,
				offset,
				static_cast<Whence>(whence));
			if (!position.ok()) {
				return ToErrno(position.error());
			}
			PutU64(*data, uint32_t(newPointer), position.value());
			return code::kSuccess;
		});
	Define(
		linker,
		defined,
		module,
		"fd_tell",
		[this](wasmtime::Caller caller, int32_t fd, int32_t pointer) -> int32_t {
			const auto data = ViewOf(caller);
			const auto slot = find(fd);
			if (!data) {
				return code::kFault;
			} else if (!slot) {
				return code::kBadf;
			} else if (slot->kind == Fd::Kind::Stdio) {
				return code::kSPipe;
			} else if (!InRange(*data, uint32_t(pointer), 8)) {
				return code::kFault;
			}
			const auto position = _session->tell(slot->handle);
			if (!position.ok()) {
				return ToErrno(position.error());
			}
			PutU64(*data, uint32_t(pointer), position.value());
			return code::kSuccess;
		});
	Define(
		linker,
		defined,
		module,
		"fd_filestat_get",
		[this](wasmtime::Caller caller, int32_t fd, int32_t pointer) -> int32_t {
			const auto data = ViewOf(caller);
			const auto slot = find(fd);
			if (!data) {
				return code::kFault;
			} else if (!slot) {
				return code::kBadf;
			} else if (!InRange(*data, uint32_t(pointer), 64)) {
				return code::kFault;
			} else if (slot->kind == Fd::Kind::Stdio) {
				WriteFilestat(
					*data,
					uint32_t(pointer),
					filetype::kCharacterDevice,
					uint64_t(fd) + 1,
					0);
				return code::kSuccess;
			}
			const auto stat = _session->stat(slot->handle);
			if (!stat.ok()) {
				return ToErrno(stat.error());
			}
			WriteFilestat(
				*data,
				uint32_t(pointer),
				FiletypeOf(stat.value().directory),
				uint64_t(stat.value().id) + 1000,
				stat.value().size);
			return code::kSuccess;
		});
}

void Wasi::linkDirectories(
		wasmtime::Linker &linker,
		std::unordered_set<std::string> &defined) {
	constexpr auto module = std::string_view("wasi_snapshot_preview1");
	Define(
		linker,
		defined,
		module,
		"path_open",
		[this](
			wasmtime::Caller caller,
			int32_t baseFd,
			int32_t,
			int32_t pathPointer,
			int32_t pathLength,
			int32_t openFlags,
			int64_t rightsBase,
			int64_t,
			int32_t,
			int32_t openedPointer) -> int32_t {
			const auto data = ViewOf(caller);
			const auto base = find(baseFd);
			if (!data) {
				return code::kFault;
			} else if (!base) {
				return code::kBadf;
			} else if (base->kind == Fd::Kind::Stdio) {
				return code::kNotDir;
			} else if (!InRange(*data, uint32_t(openedPointer), 4)) {
				return code::kFault;
			}
			auto path = std::string_view();
			if (const auto error = ReadPath(
					*data,
					pathPointer,
					pathLength,
					path)
				; error != code::kSuccess) {
				return error;
			} else if ((openFlags & (1 | 4 | 8)) != 0
				|| (uint64_t(rightsBase) & right::kWrite) != 0) {
				return code::kRoFs;
			}
			const auto opened = _session->open(base->handle, path);
			if (!opened.ok()) {
				return ToErrno(opened.error());
			}
			if ((openFlags & 2) != 0) {
				const auto stat = _session->stat(opened.value());
				if (!stat.ok() || !stat.value().directory) {
					const auto closed = _session->close(opened.value());
					return (closed == Errc::Ok) ? code::kNotDir : ToErrno(closed);
				}
			}
			PutU32(
				*data,
				uint32_t(openedPointer),
				uint32_t(allocate(opened.value())));
			return code::kSuccess;
		});
	Define(
		linker,
		defined,
		module,
		"path_filestat_get",
		[this](
			wasmtime::Caller caller,
			int32_t baseFd,
			int32_t,
			int32_t pathPointer,
			int32_t pathLength,
			int32_t pointer) -> int32_t {
			const auto data = ViewOf(caller);
			const auto base = find(baseFd);
			if (!data) {
				return code::kFault;
			} else if (!base) {
				return code::kBadf;
			} else if (base->kind == Fd::Kind::Stdio) {
				return code::kNotDir;
			} else if (!InRange(*data, uint32_t(pointer), 64)) {
				return code::kFault;
			}
			auto path = std::string_view();
			if (const auto error = ReadPath(
					*data,
					pathPointer,
					pathLength,
					path)
				; error != code::kSuccess) {
				return error;
			}
			const auto stat = _session->statPath(base->handle, path);
			if (!stat.ok()) {
				return ToErrno(stat.error());
			}
			WriteFilestat(
				*data,
				uint32_t(pointer),
				FiletypeOf(stat.value().directory),
				uint64_t(stat.value().id) + 1000,
				stat.value().size);
			return code::kSuccess;
		});
	Define(
		linker,
		defined,
		module,
		"path_readlink",
		[this](
			wasmtime::Caller caller,
			int32_t baseFd,
			int32_t pathPointer,
			int32_t pathLength,
			int32_t,
			int32_t,
			int32_t usedPointer) -> int32_t {
			const auto data = ViewOf(caller);
			const auto base = find(baseFd);
			if (!data) {
				return code::kFault;
			} else if (!base) {
				return code::kBadf;
			} else if (base->kind == Fd::Kind::Stdio) {
				return code::kNotDir;
			} else if (!InRange(*data, uint32_t(usedPointer), 4)) {
				return code::kFault;
			}
			auto path = std::string_view();
			if (const auto error = ReadPath(
					*data,
					pathPointer,
					pathLength,
					path)
				; error != code::kSuccess) {
				return error;
			}
			const auto stat = _session->statPath(base->handle, path);
			return stat.ok() ? code::kInval : ToErrno(stat.error());
		});
	Define(
		linker,
		defined,
		module,
		"fd_readdir",
		[this](
			wasmtime::Caller caller,
			int32_t fd,
			int32_t bufferPointer,
			int32_t bufferLength,
			int64_t cookie,
			int32_t usedPointer) -> int32_t {
			const auto data = ViewOf(caller);
			const auto slot = find(fd);
			if (!data) {
				return code::kFault;
			} else if (!slot) {
				return code::kBadf;
			} else if (slot->kind == Fd::Kind::Stdio) {
				return code::kNotDir;
			} else if (bufferLength < 0) {
				return code::kInval;
			} else if (!InRange(*data, uint32_t(bufferPointer), uint32_t(bufferLength))
				|| !InRange(*data, uint32_t(usedPointer), 4)) {
				return code::kFault;
			}
			const auto capacity = size_t(bufferLength);
			auto used = size_t(0);
			auto next = uint64_t(cookie);
			while (used < capacity) {
				const auto chunk = _session->readDirectory(slot->handle, next, 32);
				if (!chunk.ok()) {
					if (used == 0) {
						return ToErrno(chunk.error());
					}
					break;
				} else if (chunk.value().empty()) {
					break;
				}
				for (const auto &entry : chunk.value()) {
					auto bytes = std::vector<uint8_t>(
						kDirentHeader + entry.name.size());
					const auto put = [&](size_t at, uint64_t value, int width) {
						for (auto i = 0; i != width; ++i) {
							bytes[at + size_t(i)] = uint8_t(value >> (8 * i));
						}
					};
					put(0, entry.next, 8);
					put(8, uint64_t(entry.id) + 1000, 8);
					put(16, entry.name.size(), 4);
					put(20, entry.directory
						? filetype::kDirectory
						: filetype::kRegularFile, 1);
					std::memcpy(
						bytes.data() + kDirentHeader,
						entry.name.data(),
						entry.name.size());
					const auto written = std::min(capacity - used, bytes.size());
					std::memcpy(
						data->data() + uint32_t(bufferPointer) + used,
						bytes.data(),
						written);
					used += written;
					if (written < bytes.size()) {
						break;
					}
					next = entry.next;
				}
			}
			PutU32(*data, uint32_t(usedPointer), uint32_t(used));
			return code::kSuccess;
		});
}

void Wasi::linkRefusals(
		wasmtime::Linker &linker,
		std::unordered_set<std::string> &defined) {
	constexpr auto module = std::string_view("wasi_snapshot_preview1");
	const auto readOnly = [&](std::string_view name, auto function) {
		Define(linker, defined, module, name, function);
	};
	readOnly("path_create_directory", [](int32_t, int32_t, int32_t) -> int32_t {
		return code::kRoFs;
	});
	readOnly("path_remove_directory", [](int32_t, int32_t, int32_t) -> int32_t {
		return code::kRoFs;
	});
	readOnly("path_unlink_file", [](int32_t, int32_t, int32_t) -> int32_t {
		return code::kRoFs;
	});
	readOnly("path_rename", [](int32_t, int32_t, int32_t, int32_t, int32_t, int32_t)
	-> int32_t {
		return code::kRoFs;
	});
	readOnly("path_link", [](int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t)
	-> int32_t {
		return code::kRoFs;
	});
	readOnly("path_symlink", [](int32_t, int32_t, int32_t, int32_t, int32_t)
	-> int32_t {
		return code::kRoFs;
	});
	readOnly("fd_allocate", [](int32_t, int64_t, int64_t) -> int32_t {
		return code::kRoFs;
	});
	readOnly("fd_filestat_set_size", [](int32_t, int64_t) -> int32_t {
		return code::kRoFs;
	});
	readOnly("fd_filestat_set_times", [](int32_t, int64_t, int64_t, int32_t)
	-> int32_t {
		return code::kRoFs;
	});
	readOnly("path_filestat_set_times", [](
			int32_t,
			int32_t,
			int32_t,
			int32_t,
			int64_t,
			int64_t,
			int32_t) -> int32_t {
		return code::kRoFs;
	});
	readOnly("fd_pwrite", [](int32_t, int32_t, int32_t, int64_t, int32_t)
	-> int32_t {
		return code::kBadf;
	});
	readOnly("fd_renumber", [](int32_t, int32_t) -> int32_t {
		return code::kNoSys;
	});
	readOnly("fd_fdstat_set_rights", [](int32_t, int64_t, int64_t) -> int32_t {
		return code::kNoSys;
	});
	readOnly("poll_oneoff", [](int32_t, int32_t, int32_t, int32_t) -> int32_t {
		return code::kNoSys;
	});
	readOnly("proc_raise", [](int32_t) -> int32_t {
		return code::kNoSys;
	});
	readOnly("sock_accept", [](int32_t, int32_t, int32_t) -> int32_t {
		return code::kNotSup;
	});
	readOnly("sock_recv", [](int32_t, int32_t, int32_t, int32_t, int32_t, int32_t)
	-> int32_t {
		return code::kNotSup;
	});
	readOnly("sock_send", [](int32_t, int32_t, int32_t, int32_t, int32_t)
	-> int32_t {
		return code::kNotSup;
	});
	readOnly("sock_shutdown", [](int32_t, int32_t) -> int32_t {
		return code::kNotSup;
	});
	const auto harmless = [this](int32_t fd) {
		return find(fd) ? code::kSuccess : code::kBadf;
	};
	readOnly("fd_sync", harmless);
	readOnly("fd_datasync", harmless);
	readOnly("fd_advise", [this](int32_t fd, int64_t, int64_t, int32_t)
	-> int32_t {
		return find(fd) ? code::kSuccess : code::kBadf;
	});
	readOnly("fd_fdstat_set_flags", [this](int32_t fd, int32_t) -> int32_t {
		return find(fd) ? code::kSuccess : code::kBadf;
	});
}

} // namespace fgpl
