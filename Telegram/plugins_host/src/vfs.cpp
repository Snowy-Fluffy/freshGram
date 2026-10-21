#include "vfs.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace fgpl {
namespace {

constexpr std::string_view kForbidden = "/\\:*?\"<>|";

[[nodiscard]] std::string Lowered(std::string_view name) {
	auto result = std::string(name);
	for (auto &ch : result) {
		if (ch >= 'A' && ch <= 'Z') {
			ch = char(ch - 'A' + 'a');
		}
	}
	return result;
}

} // namespace

bool IsValidName(std::string_view name, size_t maxLength) {
	if (name.empty()
		|| name.size() > maxLength
		|| name == "."
		|| name == ".."
		|| name.front() == ' '
		|| name.back() == ' '
		|| name.back() == '.') {
		return false;
	}
	for (const auto ch : name) {
		const auto code = uint8_t(ch);
		if (code < 0x20
			|| code > 0x7E
			|| kForbidden.find(ch) != std::string_view::npos) {
			return false;
		}
	}
	return true;
}

Result<ParsedPath, Errc> ParsePath(
		std::string_view path,
		const VfsLimits &limits) {
	if (path.empty()) {
		return Errc::NotFound;
	} else if (path.size() > limits.maxPathLength) {
		return Errc::NameTooLong;
	} else if (path.find('\0') != std::string_view::npos) {
		return Errc::InvalidArgument;
	} else if (path.front() == '/') {
		return Errc::NotCapable;
	}
	auto result = ParsedPath();
	result.directoryOnly = (path.back() == '/');
	auto depth = size_t(0);
	while (!path.empty()) {
		const auto slash = path.find('/');
		const auto part = path.substr(0, slash);
		path = (slash == std::string_view::npos)
			? std::string_view()
			: path.substr(slash + 1);
		if (part.empty() || part == ".") {
			continue;
		} else if (part.size() > limits.maxNameLength) {
			return Errc::NameTooLong;
		} else if (part == "..") {
			if (depth == 0) {
				return Errc::NotCapable;
			}
			--depth;
		} else if (++depth > limits.maxDepth) {
			return Errc::NameTooLong;
		}
		result.parts.push_back(part);
	}
	return result;
}

std::optional<NodeId> Vfs::child(
		NodeId directory,
		std::string_view name) const {
	const auto &list = _nodes[directory].children;
	const auto i = std::lower_bound(
		list.begin(),
		list.end(),
		name,
		[&](NodeId id, std::string_view value) {
			return _nodes[id].name < value;
		});
	if (i != list.end() && _nodes[*i].name == name) {
		return *i;
	}
	return std::nullopt;
}

Result<NodeId, Errc> Vfs::resolve(
		NodeId base,
		std::string_view path) const {
	if (base >= _nodes.size()) {
		return Errc::BadHandle;
	} else if (!_nodes[base].directory) {
		return Errc::NotDirectory;
	}
	const auto parsed = ParsePath(path, _limits);
	if (!parsed.ok()) {
		return parsed.error();
	}
	auto stack = std::vector<NodeId>{ base };
	for (const auto part : parsed.value().parts) {
		if (!_nodes[stack.back()].directory) {
			return Errc::NotDirectory;
		} else if (part == "..") {
			stack.pop_back();
			continue;
		}
		const auto found = child(stack.back(), part);
		if (!found) {
			return Errc::NotFound;
		}
		stack.push_back(*found);
	}
	if (parsed.value().directoryOnly && !_nodes[stack.back()].directory) {
		return Errc::NotDirectory;
	}
	return stack.back();
}

bool Vfs::isDirectory(NodeId node) const {
	return _nodes[node].directory;
}

uint64_t Vfs::size(NodeId node) const {
	return _nodes[node].data.size();
}

std::span<const uint8_t> Vfs::contents(NodeId node) const {
	return _nodes[node].data;
}

std::span<const NodeId> Vfs::children(NodeId node) const {
	return _nodes[node].children;
}

const std::string &Vfs::name(NodeId node) const {
	return _nodes[node].name;
}

bool Vfs::within(NodeId node, NodeId ancestor) const {
	for (;;) {
		if (node == ancestor) {
			return true;
		} else if (node == kRoot) {
			return false;
		}
		node = _nodes[node].parent;
	}
}

VfsBuilder::VfsBuilder(const VfsLimits &limits)
: _vfs(std::shared_ptr<Vfs>(new Vfs(limits))) {
	_vfs->_nodes.emplace_back();
	_lower.emplace_back();
}

std::optional<std::vector<std::string_view>> VfsBuilder::SplitBuildPath(
		std::string_view path,
		const VfsLimits &limits) {
	if (path.empty() || path.size() > limits.maxPathLength) {
		return std::nullopt;
	}
	auto parts = std::vector<std::string_view>();
	while (true) {
		const auto slash = path.find('/');
		const auto part = path.substr(0, slash);
		if (!IsValidName(part, limits.maxNameLength)) {
			return std::nullopt;
		}
		parts.push_back(part);
		if (slash == std::string_view::npos) {
			break;
		}
		path = path.substr(slash + 1);
	}
	if (parts.size() > limits.maxDepth) {
		return std::nullopt;
	}
	return parts;
}

Errc VfsBuilder::ensureDirectory(
		const std::vector<std::string_view> &parts,
		NodeId &out) {
	auto current = Vfs::kRoot;
	for (const auto part : parts) {
		const auto key = Lowered(part);
		const auto found = _lower[current].find(key);
		if (found != _lower[current].end()) {
			if (!_vfs->_nodes[found->second].directory
				|| _vfs->_nodes[found->second].name != part) {
				return Errc::AlreadyExists;
			}
			current = found->second;
			continue;
		} else if (_directoryCount >= _vfs->_limits.maxDirectories) {
			return Errc::LimitExceeded;
		}
		const auto id = NodeId(_vfs->_nodes.size());
		auto &node = _vfs->_nodes.emplace_back();
		node.name = std::string(part);
		node.parent = current;
		_lower.emplace_back();
		_lower[current].emplace(key, id);
		_vfs->_nodes[current].children.push_back(id);
		++_directoryCount;
		current = id;
	}
	out = current;
	return Errc::Ok;
}

Errc VfsBuilder::addDirectory(std::string_view path) {
	const auto parts = SplitBuildPath(path, _vfs->_limits);
	if (!parts) {
		return Errc::InvalidArgument;
	}
	auto id = NodeId();
	return ensureDirectory(*parts, id);
}

Errc VfsBuilder::addFile(std::string_view path, std::vector<uint8_t> data) {
	auto parts = SplitBuildPath(path, _vfs->_limits);
	if (!parts) {
		return Errc::InvalidArgument;
	} else if (_vfs->_fileCount >= _vfs->_limits.maxFiles
		|| data.size() > _vfs->_limits.maxTotalBytes - _vfs->_totalBytes) {
		return Errc::LimitExceeded;
	}
	const auto name = parts->back();
	parts->pop_back();
	auto directory = NodeId();
	if (const auto error = ensureDirectory(*parts, directory)
		; error != Errc::Ok) {
		return error;
	}
	const auto key = Lowered(name);
	if (_lower[directory].contains(key)) {
		return Errc::AlreadyExists;
	}
	const auto id = NodeId(_vfs->_nodes.size());
	auto &node = _vfs->_nodes.emplace_back();
	node.name = std::string(name);
	node.parent = directory;
	node.directory = false;
	_vfs->_totalBytes += data.size();
	node.data = std::move(data);
	_lower.emplace_back();
	_lower[directory].emplace(key, id);
	_vfs->_nodes[directory].children.push_back(id);
	++_vfs->_fileCount;
	return Errc::Ok;
}

std::shared_ptr<const Vfs> VfsBuilder::finish() {
	auto &nodes = _vfs->_nodes;
	for (auto &node : nodes) {
		std::sort(
			node.children.begin(),
			node.children.end(),
			[&](NodeId a, NodeId b) {
				return nodes[a].name < nodes[b].name;
			});
	}
	_lower.clear();
	return std::exchange(_vfs, nullptr);
}

VfsSession::VfsSession(std::shared_ptr<const Vfs> vfs)
: _vfs(std::move(vfs)) {
}

const VfsSession::Slot *VfsSession::find(Handle handle) const {
	if (handle == 0 || handle > _slots.size()) {
		return nullptr;
	}
	const auto &slot = _slots[handle - 1];
	return slot.used ? &slot : nullptr;
}

VfsSession::Slot *VfsSession::find(Handle handle) {
	return const_cast<Slot*>(std::as_const(*this).find(handle));
}

Result<VfsSession::Handle, Errc> VfsSession::openNode(NodeId node) {
	if (_open >= _vfs->limits().maxOpenHandles) {
		return Errc::TooManyOpen;
	}
	auto index = size_t(0);
	while (index != _slots.size() && _slots[index].used) {
		++index;
	}
	if (index == _slots.size()) {
		_slots.emplace_back();
	}
	_slots[index] = Slot{ .used = true, .node = node };
	++_open;
	return Handle(index + 1);
}

Result<VfsSession::Handle, Errc> VfsSession::open(
		Handle base,
		std::string_view path) {
	const auto slot = find(base);
	if (!slot) {
		return Errc::BadHandle;
	}
	const auto node = _vfs->resolve(slot->node, path);
	if (!node.ok()) {
		return node.error();
	}
	return openNode(node.value());
}

Errc VfsSession::close(Handle handle) {
	const auto slot = find(handle);
	if (!slot) {
		return Errc::BadHandle;
	}
	*slot = Slot();
	--_open;
	return Errc::Ok;
}

Result<size_t, Errc> VfsSession::readAt(
		Handle handle,
		uint64_t offset,
		std::span<uint8_t> out) const {
	const auto slot = find(handle);
	if (!slot) {
		return Errc::BadHandle;
	} else if (_vfs->isDirectory(slot->node)) {
		return Errc::IsDirectory;
	}
	const auto data = _vfs->contents(slot->node);
	if (offset >= data.size()) {
		return size_t(0);
	}
	const auto count = std::min<uint64_t>(out.size(), data.size() - offset);
	std::copy_n(data.begin() + ptrdiff_t(offset), size_t(count), out.begin());
	return size_t(count);
}

Result<size_t, Errc> VfsSession::read(
		Handle handle,
		std::span<uint8_t> out) {
	const auto slot = find(handle);
	if (!slot) {
		return Errc::BadHandle;
	}
	const auto count = readAt(handle, slot->position, out);
	if (count.ok()) {
		slot->position += count.value();
	}
	return count;
}

Result<uint64_t, Errc> VfsSession::seek(
		Handle handle,
		int64_t offset,
		Whence whence) {
	const auto slot = find(handle);
	if (!slot) {
		return Errc::BadHandle;
	} else if (_vfs->isDirectory(slot->node)) {
		return Errc::IsDirectory;
	}
	const auto origin = (whence == Whence::Set)
		? uint64_t(0)
		: (whence == Whence::Current)
		? slot->position
		: _vfs->size(slot->node);
	auto target = uint64_t(0);
	if (offset >= 0) {
		if (uint64_t(offset) > std::numeric_limits<uint64_t>::max() - origin) {
			return Errc::InvalidArgument;
		}
		target = origin + uint64_t(offset);
	} else {
		const auto back = uint64_t(-(offset + 1)) + 1;
		if (back > origin) {
			return Errc::InvalidArgument;
		}
		target = origin - back;
	}
	slot->position = target;
	return target;
}

Result<uint64_t, Errc> VfsSession::tell(Handle handle) const {
	const auto slot = find(handle);
	if (!slot) {
		return Errc::BadHandle;
	} else if (_vfs->isDirectory(slot->node)) {
		return Errc::IsDirectory;
	}
	return slot->position;
}

Result<VfsStat, Errc> VfsSession::stat(Handle handle) const {
	const auto slot = find(handle);
	if (!slot) {
		return Errc::BadHandle;
	}
	return VfsStat{
		.directory = _vfs->isDirectory(slot->node),
		.size = _vfs->size(slot->node),
		.id = slot->node,
	};
}

Result<VfsStat, Errc> VfsSession::statPath(
		Handle base,
		std::string_view path) const {
	const auto slot = find(base);
	if (!slot) {
		return Errc::BadHandle;
	}
	const auto node = _vfs->resolve(slot->node, path);
	if (!node.ok()) {
		return node.error();
	}
	return VfsStat{
		.directory = _vfs->isDirectory(node.value()),
		.size = _vfs->size(node.value()),
		.id = node.value(),
	};
}

Result<std::vector<VfsDirEntry>, Errc> VfsSession::readDirectory(
		Handle handle,
		uint64_t cookie,
		size_t maxEntries) const {
	const auto slot = find(handle);
	if (!slot) {
		return Errc::BadHandle;
	} else if (!_vfs->isDirectory(slot->node)) {
		return Errc::NotDirectory;
	}
	const auto list = _vfs->children(slot->node);
	auto result = std::vector<VfsDirEntry>();
	for (auto i = cookie; i < list.size() && result.size() < maxEntries; ++i) {
		const auto id = list[size_t(i)];
		result.push_back(VfsDirEntry{
			.next = i + 1,
			.name = _vfs->name(id),
			.directory = _vfs->isDirectory(id),
			.id = id,
		});
	}
	return result;
}

} // namespace fgpl
