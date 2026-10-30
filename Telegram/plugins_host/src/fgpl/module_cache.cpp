#include "fgpl/module_cache.h"

#include "fgpl/os.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <system_error>

namespace fgpl {
namespace {

constexpr char kMagic[8] = { 'F', 'G', 'P', 'L', 'C', 'W', 'M', '1' };
constexpr auto kMaxFileBytes = uint64_t(512) << 20;

void PutU64(std::vector<uint8_t> &out, uint64_t value) {
	for (auto i = 0; i != 8; ++i) {
		out.push_back(uint8_t(value >> (8 * i)));
	}
}

[[nodiscard]] uint64_t GetU64(const uint8_t *data) {
	auto result = uint64_t(0);
	for (auto i = 0; i != 8; ++i) {
		result |= uint64_t(data[i]) << (8 * i);
	}
	return result;
}

[[nodiscard]] std::string Hex(const Digest &digest) {
	constexpr auto digits = "0123456789abcdef";
	auto result = std::string();
	for (const auto byte : digest) {
		result.push_back(digits[byte >> 4]);
		result.push_back(digits[byte & 15]);
	}
	return result;
}

} // namespace

ModuleCache::ModuleCache(std::filesystem::path directory, CacheKey key)
: _directory(std::move(directory))
, _key(std::move(key)) {
}

std::filesystem::path ModuleCache::pathFor(
		std::span<const uint8_t> wasm) const {
	return _directory / (Hex(Sha256(wasm)) + ".cwasm");
}

Digest ModuleCache::seal(std::span<const uint8_t> content) const {
	return HmacSha256(_key.secret, content);
}

std::optional<std::vector<uint8_t>> ModuleCache::find(
		std::span<const uint8_t> wasm) const {
	const auto path = pathFor(wasm);
	auto error = std::error_code();
	const auto size = std::filesystem::file_size(path, error);
	if (error || size < 8 + 4 + 32 + 8 + 32 || size > kMaxFileBytes) {
		return std::nullopt;
	}
	auto stream = std::ifstream(path, std::ios::binary);
	auto bytes = std::vector<uint8_t>(size_t(size));
	if (!stream.read(
			reinterpret_cast<char *>(bytes.data()),
			std::streamsize(bytes.size()))) {
		return std::nullopt;
	}
	const auto body = std::span<const uint8_t>(bytes.data(), bytes.size() - 32);
	const auto expected = seal(body);
	if (!ConstantTimeEqual(
			expected,
			std::span<const uint8_t>(bytes.data() + bytes.size() - 32, 32))) {
		return std::nullopt;
	}
	if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
		return std::nullopt;
	}
	auto at = size_t(8);
	auto prefix = uint32_t(0);
	std::memcpy(&prefix, bytes.data() + at, 4);
	at += 4;
	if (prefix != _key.fingerprint.size()
		|| at + prefix + 32 + 8 > body.size()
		|| std::memcmp(bytes.data() + at, _key.fingerprint.data(), prefix) != 0) {
		return std::nullopt;
	}
	at += prefix;
	if (!ConstantTimeEqual(
			std::span<const uint8_t>(bytes.data() + at, 32),
			Sha256(wasm))) {
		return std::nullopt;
	}
	at += 32;
	const auto payload = GetU64(bytes.data() + at);
	at += 8;
	if (payload != body.size() - at) {
		return std::nullopt;
	}
	return std::vector<uint8_t>(bytes.begin() + ptrdiff_t(at), bytes.begin() + ptrdiff_t(body.size()));
}

bool ModuleCache::store(
		std::span<const uint8_t> wasm,
		std::span<const uint8_t> compiled) const {
	auto error = std::error_code();
	std::filesystem::create_directories(_directory, error);
	if (error) {
		return false;
	}
	auto content = std::vector<uint8_t>(kMagic, kMagic + sizeof(kMagic));
	const auto prefix = uint32_t(_key.fingerprint.size());
	for (auto i = 0; i != 4; ++i) {
		content.push_back(uint8_t(prefix >> (8 * i)));
	}
	content.insert(
		content.end(),
		_key.fingerprint.begin(),
		_key.fingerprint.end());
	const auto hash = Sha256(wasm);
	content.insert(content.end(), hash.begin(), hash.end());
	PutU64(content, compiled.size());
	content.insert(content.end(), compiled.begin(), compiled.end());
	const auto mac = seal(content);
	content.insert(content.end(), mac.begin(), mac.end());

	const auto target = pathFor(wasm);
	static auto counter = std::atomic<uint64_t>(0);
	const auto temporary = std::filesystem::path(target) += (
		"." + std::to_string(os::ProcessId())
		+ "." + std::to_string(counter.fetch_add(1))
		+ ".tmp");
	{
		auto stream = std::ofstream(temporary, std::ios::binary | std::ios::trunc);
		if (!stream.write(
				reinterpret_cast<const char *>(content.data()),
				std::streamsize(content.size()))) {
			return false;
		}
	}
	if (!os::ReplaceFileAtomic(temporary, target, error)) {
		std::filesystem::remove(temporary, error);
		return false;
	}
	return true;
}

void ModuleCache::erase(std::span<const uint8_t> wasm) const {
	auto error = std::error_code();
	std::filesystem::remove(pathFor(wasm), error);
}

void ModuleCache::prune(uint64_t maxTotalBytes) const {
	struct Entry {
		std::filesystem::path path;
		uint64_t size = 0;
		std::filesystem::file_time_type time;
	};
	auto entries = std::vector<Entry>();
	auto total = uint64_t(0);
	auto error = std::error_code();
	for (const auto &item : std::filesystem::directory_iterator(_directory, error)) {
		if (item.path().extension() != ".cwasm") {
			continue;
		}
		auto entry = Entry();
		entry.path = item.path();
		entry.size = item.file_size(error);
		entry.time = item.last_write_time(error);
		total += entry.size;
		entries.push_back(std::move(entry));
	}
	std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
		return a.time < b.time;
	});
	for (const auto &entry : entries) {
		if (total <= maxTotalBytes) {
			break;
		}
		std::filesystem::remove(entry.path, error);
		total -= entry.size;
	}
}

} // namespace fgpl
