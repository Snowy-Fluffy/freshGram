#pragma once

#include "result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fgpl {

enum class Errc : uint8_t {
	Ok,
	NotFound,
	NotCapable,
	NotDirectory,
	IsDirectory,
	InvalidArgument,
	NameTooLong,
	TooManyOpen,
	BadHandle,
	AlreadyExists,
	LimitExceeded,
};

struct VfsLimits {
	size_t maxFiles = 16384;
	size_t maxDirectories = 4096;
	size_t maxDepth = 32;
	size_t maxNameLength = 128;
	size_t maxPathLength = 512;
	uint64_t maxTotalBytes = uint64_t(256) << 20;
	size_t maxOpenHandles = 64;
};

using NodeId = uint32_t;

struct ParsedPath {
	std::vector<std::string_view> parts;
	bool directoryOnly = false;
};

[[nodiscard]] Result<ParsedPath, Errc> ParsePath(
	std::string_view path,
	const VfsLimits &limits);

[[nodiscard]] bool IsValidName(std::string_view name, size_t maxLength);

class VfsBuilder;

class Vfs {
public:
	static constexpr auto kRoot = NodeId(0);

	[[nodiscard]] Result<NodeId, Errc> resolve(
		NodeId base,
		std::string_view path) const;

	[[nodiscard]] bool isDirectory(NodeId node) const;
	[[nodiscard]] uint64_t size(NodeId node) const;
	[[nodiscard]] std::span<const uint8_t> contents(NodeId node) const;
	[[nodiscard]] std::span<const NodeId> children(NodeId node) const;
	[[nodiscard]] const std::string &name(NodeId node) const;
	[[nodiscard]] bool within(NodeId node, NodeId ancestor) const;

	[[nodiscard]] uint64_t totalBytes() const {
		return _totalBytes;
	}
	[[nodiscard]] size_t fileCount() const {
		return _fileCount;
	}
	[[nodiscard]] const VfsLimits &limits() const {
		return _limits;
	}

private:
	friend class VfsBuilder;

	struct Node {
		std::string name;
		NodeId parent = 0;
		bool directory = true;
		std::vector<NodeId> children;
		std::vector<uint8_t> data;
	};

	explicit Vfs(const VfsLimits &limits) : _limits(limits) {
	}

	[[nodiscard]] std::optional<NodeId> child(
		NodeId directory,
		std::string_view name) const;

	VfsLimits _limits;
	std::vector<Node> _nodes;
	uint64_t _totalBytes = 0;
	size_t _fileCount = 0;

};

class VfsBuilder {
public:
	explicit VfsBuilder(const VfsLimits &limits = {});

	[[nodiscard]] Errc addFile(
		std::string_view path,
		std::vector<uint8_t> data);
	[[nodiscard]] Errc addDirectory(std::string_view path);

	[[nodiscard]] std::shared_ptr<const Vfs> finish();

private:
	[[nodiscard]] Errc ensureDirectory(
		const std::vector<std::string_view> &parts,
		NodeId &out);
	[[nodiscard]] static std::optional<std::vector<std::string_view>>
	SplitBuildPath(std::string_view path, const VfsLimits &limits);

	std::shared_ptr<Vfs> _vfs;
	std::vector<std::unordered_map<std::string, NodeId>> _lower;
	size_t _directoryCount = 1;

};

struct VfsStat {
	bool directory = false;
	uint64_t size = 0;
	NodeId id = 0;
};

struct VfsDirEntry {
	uint64_t next = 0;
	std::string name;
	bool directory = false;
	NodeId id = 0;
};

enum class Whence : uint8_t {
	Set,
	Current,
	End,
};

class VfsSession {
public:
	using Handle = uint32_t;

	explicit VfsSession(std::shared_ptr<const Vfs> vfs);

	[[nodiscard]] Result<Handle, Errc> openNode(NodeId node);
	[[nodiscard]] Result<Handle, Errc> open(
		Handle base,
		std::string_view path);
	[[nodiscard]] Errc close(Handle handle);

	[[nodiscard]] Result<size_t, Errc> read(
		Handle handle,
		std::span<uint8_t> out);
	[[nodiscard]] Result<size_t, Errc> readAt(
		Handle handle,
		uint64_t offset,
		std::span<uint8_t> out) const;
	[[nodiscard]] Result<uint64_t, Errc> seek(
		Handle handle,
		int64_t offset,
		Whence whence);
	[[nodiscard]] Result<uint64_t, Errc> tell(Handle handle) const;

	[[nodiscard]] Result<VfsStat, Errc> stat(Handle handle) const;
	[[nodiscard]] Result<VfsStat, Errc> statPath(
		Handle base,
		std::string_view path) const;
	[[nodiscard]] Result<std::vector<VfsDirEntry>, Errc> readDirectory(
		Handle handle,
		uint64_t cookie,
		size_t maxEntries) const;

	[[nodiscard]] size_t openCount() const {
		return _open;
	}

private:
	struct Slot {
		bool used = false;
		NodeId node = 0;
		uint64_t position = 0;
	};

	[[nodiscard]] const Slot *find(Handle handle) const;
	[[nodiscard]] Slot *find(Handle handle);

	std::shared_ptr<const Vfs> _vfs;
	std::vector<Slot> _slots;
	size_t _open = 0;

};

} // namespace fgpl
