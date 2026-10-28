#include "fgpl/file_storage.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fgpl {
namespace {

enum class Loaded {
	Missing,
	Ok,
	Corrupt,
};

[[nodiscard]] Loaded Load(
		const std::filesystem::path &file,
		std::map<std::string, cbor::Bytes> &items) {
	auto stream = std::ifstream(file, std::ios::binary);
	if (!stream) {
		return Loaded::Missing;
	}
	const auto bytes = cbor::Bytes(
		std::istreambuf_iterator<char>(stream),
		std::istreambuf_iterator<char>());
	auto limits = cbor::Limits();
	limits.maxItems = size_t(1) << 20;
	limits.maxContainer = size_t(1) << 20;
	limits.maxString = size_t(64) << 20;
	limits.maxBytes = size_t(64) << 20;
	const auto decoded = cbor::Decode(bytes, limits);
	const auto found = decoded.ok() ? decoded.value().find("items") : nullptr;
	if (!found || !found->asMap()) {
		return Loaded::Corrupt;
	}
	for (const auto &entry : *found->asMap()) {
		if (const auto value = entry.value.asBytes()) {
			items[entry.key] = *value;
		}
	}
	return Loaded::Ok;
}

[[nodiscard]] bool WriteSynced(
		const std::filesystem::path &file,
		const cbor::Bytes &bytes) {
	const auto fd = ::open(
		file.c_str(),
		O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
		0600);
	if (fd < 0) {
		return false;
	}
	auto written = size_t(0);
	while (written < bytes.size()) {
		const auto result = ::write(
			fd,
			bytes.data() + written,
			bytes.size() - written);
		if (result < 0 && errno == EINTR) {
			continue;
		} else if (result <= 0) {
			::close(fd);
			return false;
		}
		written += size_t(result);
	}
	const auto synced = (::fsync(fd) == 0);
	return (::close(fd) == 0) && synced;
}

void SyncDirectory(const std::filesystem::path &directory) {
	const auto fd = ::open(
		directory.c_str(),
		O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		::fsync(fd);
		::close(fd);
	}
}

} // namespace

FileStorage::FileStorage(std::filesystem::path file)
: _file(std::move(file)) {
	const auto backup = _file.string() + ".bak";
	auto error = std::error_code();
	const auto main = Load(_file, _items);
	if (main == Loaded::Ok) {
		for (const auto &item : _items) {
			_used += item.first.size() + item.second.size();
		}
		return;
	}
	if (main == Loaded::Corrupt) {
		_healthy = false;
		std::filesystem::rename(_file, _file.string() + ".corrupt", error);
		_items.clear();
	}
	// The main file is damaged or missing, use the previous good copy.
	if (Load(backup, _items) == Loaded::Ok) {
		for (const auto &item : _items) {
			_used += item.first.size() + item.second.size();
		}
		_recovered = true;
		_dirty = true;
	} else {
		_items.clear();
	}
}

FileStorage::~FileStorage() {
	flush();
}

std::optional<cbor::Bytes> FileStorage::get(const std::string &key) {
	const auto i = _items.find(key);
	if (i == _items.end()) {
		return std::nullopt;
	}
	return i->second;
}

void FileStorage::set(const std::string &key, cbor::Bytes value) {
	const auto i = _items.find(key);
	if (i != _items.end()) {
		_used -= i->first.size() + i->second.size();
		i->second = std::move(value);
		_used += i->first.size() + i->second.size();
	} else {
		_used += key.size() + value.size();
		_items.emplace(key, std::move(value));
	}
	_dirty = true;
}

bool FileStorage::remove(const std::string &key) {
	const auto i = _items.find(key);
	if (i == _items.end()) {
		return false;
	}
	_used -= i->first.size() + i->second.size();
	_items.erase(i);
	_dirty = true;
	return true;
}

std::vector<std::string> FileStorage::keys(
		const std::string &prefix,
		size_t limit) {
	auto result = std::vector<std::string>();
	for (auto i = _items.lower_bound(prefix);
		i != _items.end() && result.size() < limit;
		++i) {
		if (i->first.rfind(prefix, 0) != 0) {
			break;
		}
		result.push_back(i->first);
	}
	return result;
}

uint64_t FileStorage::usedBytes() {
	return _used;
}

size_t FileStorage::count() {
	return _items.size();
}

bool FileStorage::flush() {
	if (!_dirty) {
		return true;
	}
	auto items = cbor::Map();
	for (const auto &item : _items) {
		items.push_back({ item.first, cbor::Value::FromBytes(item.second) });
	}
	auto bytes = cbor::Bytes();
	const auto document = cbor::Value::FromMap({
		{ "v", cbor::Value::FromInt(1) },
		{ "items", cbor::Value::FromMap(std::move(items)) },
	});
	if (!cbor::Encode(document, bytes, 4)) {
		return false;
	}
	auto error = std::error_code();
	std::filesystem::create_directories(_file.parent_path(), error);
	const auto temporary = std::filesystem::path(_file.string() + "."
		+ std::to_string(getpid()) + "."
		+ std::to_string(reinterpret_cast<uintptr_t>(this)) + ".tmp");
	if (!WriteSynced(temporary, bytes)) {
		std::filesystem::remove(temporary, error);
		return false;
	}
	// Keep the previous version as a hard link, the rename leaves it intact.
	if (!_recovered && std::filesystem::exists(_file, error)) {
		const auto backup = std::filesystem::path(_file.string() + ".bak");
		std::filesystem::remove(backup, error);
		std::filesystem::create_hard_link(_file, backup, error);
		if (error) {
			std::filesystem::copy_file(
				_file,
				backup,
				std::filesystem::copy_options::overwrite_existing,
				error);
		}
	}
	std::filesystem::rename(temporary, _file, error);
	if (error) {
		std::filesystem::remove(temporary, error);
		return false;
	}
	SyncDirectory(_file.parent_path());
	_dirty = false;
	_recovered = false;
	return true;
}

} // namespace fgpl
