#include "fgpl/file_storage.h"

#include <unistd.h>

#include <fstream>
#include <iterator>
#include <system_error>

namespace fgpl {

FileStorage::FileStorage(std::filesystem::path file)
: _file(std::move(file)) {
	auto stream = std::ifstream(_file, std::ios::binary);
	if (!stream) {
		return;
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
	const auto items = decoded.ok() ? decoded.value().find("items") : nullptr;
	if (!items || !items->asMap()) {
		_healthy = false;
		auto error = std::error_code();
		std::filesystem::rename(_file, _file.string() + ".corrupt", error);
		return;
	}
	for (const auto &entry : *items->asMap()) {
		if (const auto value = entry.value.asBytes()) {
			_items[entry.key] = *value;
		}
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
	_items[key] = std::move(value);
	_dirty = true;
}

bool FileStorage::remove(const std::string &key) {
	const auto removed = _items.erase(key) != 0;
	_dirty = _dirty || removed;
	return removed;
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
	auto total = uint64_t(0);
	for (const auto &item : _items) {
		total += item.first.size() + item.second.size();
	}
	return total;
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
	const auto temporary = _file.string() + "." + std::to_string(getpid())
		+ "." + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".tmp";
	{
		auto stream = std::ofstream(temporary, std::ios::binary | std::ios::trunc);
		if (!stream.write(
				reinterpret_cast<const char *>(bytes.data()),
				std::streamsize(bytes.size()))) {
			return false;
		}
	}
	std::filesystem::rename(temporary, _file, error);
	if (error) {
		std::filesystem::remove(temporary, error);
		return false;
	}
	_dirty = false;
	return true;
}

} // namespace fgpl
