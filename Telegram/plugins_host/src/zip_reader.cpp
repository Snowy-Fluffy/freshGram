#include "zip_reader.h"

#include "vfs.h"

#include <zlib.h>

#include <algorithm>
#include <set>

namespace fgpl {
namespace {

constexpr auto kEndSignature = uint32_t(0x06054b50);
constexpr auto kCentralSignature = uint32_t(0x02014b50);
constexpr auto kLocalSignature = uint32_t(0x04034b50);
constexpr auto kDescriptorSignature = uint32_t(0x08074b50);
constexpr auto kEndSize = size_t(22);
constexpr auto kCentralSize = size_t(46);
constexpr auto kLocalSize = size_t(30);

[[nodiscard]] uint16_t U16(const uint8_t *p) {
	return uint16_t(p[0] | (p[1] << 8));
}

[[nodiscard]] uint32_t U32(const uint8_t *p) {
	return uint32_t(p[0])
		| (uint32_t(p[1]) << 8)
		| (uint32_t(p[2]) << 16)
		| (uint32_t(p[3]) << 24);
}

[[nodiscard]] ZipFailure Fail(ZipError error, std::string detail) {
	return ZipFailure{ error, std::move(detail) };
}

struct Entry {
	std::string name;
	bool directory = false;
	uint16_t flags = 0;
	uint16_t method = 0;
	uint32_t crc = 0;
	uint32_t compressed = 0;
	uint32_t size = 0;
	uint32_t offset = 0;
	uint64_t dataStart = 0;
	uint64_t end = 0;
};

[[nodiscard]] std::string Lower(const std::string &text) {
	auto result = text;
	for (auto &ch : result) {
		if (ch >= 'A' && ch <= 'Z') {
			ch = char(ch - 'A' + 'a');
		}
	}
	return result;
}

[[nodiscard]] bool ValidExtra(const uint8_t *p, size_t size) {
	auto at = size_t(0);
	while (at != size) {
		if (size - at < 4) {
			return false;
		}
		const auto tag = U16(p + at);
		const auto length = size_t(U16(p + at + 2));
		if (tag == 0x0001 || length > size - at - 4) {
			return false;
		}
		at += 4 + length;
	}
	return true;
}

} // namespace

Result<std::vector<ZipFile>, ZipFailure> ReadZip(
		std::span<const uint8_t> archive,
		const ZipLimits &limits) {
	if (archive.size() > limits.maxArchiveBytes) {
		return Fail(ZipError::TooLarge, "the archive is too large");
	} else if (archive.size() < kEndSize) {
		return Fail(ZipError::Format, "the archive is too short");
	}
	const auto data = archive.data();
	const auto endOffset = archive.size() - kEndSize;
	if (U32(data + endOffset) != kEndSignature) {
		return Fail(ZipError::Format, "no end of central directory (comments are not allowed)");
	}
	const auto end = data + endOffset;
	const auto entries = size_t(U16(end + 10));
	const auto directorySize = uint64_t(U32(end + 12));
	const auto directoryOffset = uint64_t(U32(end + 16));
	if (U16(end + 4) != 0 || U16(end + 6) != 0 || U16(end + 20) != 0) {
		return Fail(ZipError::Unsupported, "multi-disk archives or comments");
	} else if (U16(end + 8) != U16(end + 10)
		|| entries == 0xFFFF
		|| directorySize == 0xFFFFFFFF
		|| directoryOffset == 0xFFFFFFFF) {
		return Fail(ZipError::Unsupported, "zip64 archives");
	} else if (entries > limits.maxEntries) {
		return Fail(ZipError::TooLarge, "too many entries");
	} else if (directoryOffset + directorySize != endOffset) {
		return Fail(ZipError::Format, "the central directory is not where it should be");
	}

	auto list = std::vector<Entry>();
	auto position = size_t(directoryOffset);
	auto names = std::set<std::string>();
	auto filePaths = std::set<std::string>();
	auto directoryPaths = std::set<std::string>();
	auto totalSize = uint64_t(0);
	for (auto i = size_t(0); i != entries; ++i) {
		if (endOffset - position < kCentralSize
			|| U32(data + position) != kCentralSignature) {
			return Fail(ZipError::Format, "a bad central directory entry");
		}
		const auto p = data + position;
		auto entry = Entry();
		const auto madeBy = U16(p + 4);
		entry.flags = U16(p + 8);
		entry.method = U16(p + 10);
		entry.crc = U32(p + 16);
		entry.compressed = U32(p + 20);
		entry.size = U32(p + 24);
		const auto nameLength = size_t(U16(p + 28));
		const auto extraLength = size_t(U16(p + 30));
		const auto commentLength = size_t(U16(p + 32));
		const auto external = U32(p + 38);
		entry.offset = U32(p + 42);
		const auto total = kCentralSize + nameLength + extraLength + commentLength;
		if (total > endOffset - position) {
			return Fail(ZipError::Format, "a truncated central directory entry");
		} else if ((entry.flags & 0x2041) != 0) {
			return Fail(ZipError::Unsupported, "encrypted entries");
		} else if (entry.method != 0 && entry.method != 8) {
			return Fail(ZipError::Unsupported, "an unsupported compression method");
		} else if (U16(p + 34) != 0) {
			return Fail(ZipError::Unsupported, "multi-disk archives");
		} else if (entry.compressed == 0xFFFFFFFF
			|| entry.size == 0xFFFFFFFF
			|| entry.offset == 0xFFFFFFFF) {
			return Fail(ZipError::Unsupported, "zip64 entries");
		} else if (nameLength == 0 || nameLength > limits.maxNameBytes) {
			return Fail(ZipError::Unsafe, "a bad entry name length");
		} else if (!ValidExtra(p + kCentralSize + nameLength, extraLength)) {
			return Fail(ZipError::Unsupported, "a bad or zip64 extra field");
		}
		if ((madeBy >> 8) == 3) {
			const auto kind = (external >> 16) & 0xF000;
			if (kind != 0 && kind != 0x8000 && kind != 0x4000) {
				return Fail(ZipError::Unsafe, "a link or a special file");
			}
		}
		entry.name.assign(
			reinterpret_cast<const char *>(p + kCentralSize),
			nameLength);
		entry.directory = (entry.name.back() == '/');
		auto path = entry.directory
			? entry.name.substr(0, entry.name.size() - 1)
			: entry.name;
		auto start = size_t(0);
		auto depth = size_t(0);
		while (true) {
			const auto slash = path.find('/', start);
			const auto part = path.substr(
				start,
				slash == std::string::npos ? std::string::npos : slash - start);
			if (!IsValidName(part, 128)) {
				return Fail(ZipError::Unsafe, "an unsafe entry name: " + entry.name);
			}
			++depth;
			if (slash == std::string::npos) {
				break;
			}
			directoryPaths.insert(Lower(path.substr(0, slash)));
			start = slash + 1;
		}
		if (depth > limits.maxDepth) {
			return Fail(ZipError::Unsafe, "an entry is nested too deeply");
		} else if (!names.insert(Lower(path)).second) {
			return Fail(ZipError::Unsafe, "a duplicate entry: " + entry.name);
		}
		if (entry.directory) {
			directoryPaths.insert(Lower(path));
			if (entry.size != 0 || entry.compressed != 0) {
				return Fail(ZipError::Format, "a directory with content");
			}
		} else {
			filePaths.insert(Lower(path));
			if (entry.size > limits.maxEntryBytes) {
				return Fail(ZipError::TooLarge, "a file is too large: " + entry.name);
			}
			totalSize += entry.size;
			if (totalSize > limits.maxTotalBytes) {
				return Fail(ZipError::TooLarge, "the archive expands too much");
			} else if (entry.size > uint64_t(entry.compressed) * limits.maxRatio + 1024) {
				return Fail(ZipError::Unsafe, "a suspicious compression ratio: " + entry.name);
			} else if (entry.method == 0 && entry.compressed != entry.size) {
				return Fail(ZipError::Corrupt, "a stored size mismatch: " + entry.name);
			}
		}
		list.push_back(std::move(entry));
		position += total;
	}
	if (position != endOffset) {
		return Fail(ZipError::Format, "unused data in the central directory");
	}
	for (const auto &file : filePaths) {
		if (directoryPaths.contains(file)) {
			return Fail(ZipError::Unsafe, "a name is both a file and a directory");
		}
	}

	for (auto &entry : list) {
		const auto offset = size_t(entry.offset);
		if (offset > directoryOffset || directoryOffset - offset < kLocalSize
			|| U32(data + offset) != kLocalSignature) {
			return Fail(ZipError::Format, "a bad local header: " + entry.name);
		}
		const auto p = data + offset;
		const auto localFlags = U16(p + 6);
		const auto nameLength = size_t(U16(p + 26));
		const auto extraLength = size_t(U16(p + 28));
		if (nameLength != entry.name.size()
			|| kLocalSize + nameLength + extraLength > directoryOffset - offset
			|| std::string(reinterpret_cast<const char *>(p + kLocalSize), nameLength)
				!= entry.name
			|| U16(p + 8) != entry.method
			|| (localFlags & 0x2049) != (entry.flags & 0x2049)
			|| (localFlags & 8) != (entry.flags & 8)) {
			return Fail(ZipError::Format, "the local header disagrees: " + entry.name);
		} else if (!ValidExtra(p + kLocalSize + nameLength, extraLength)) {
			return Fail(ZipError::Unsupported, "a bad local extra field");
		}
		if ((entry.flags & 8) == 0
			&& (U32(p + 14) != entry.crc
				|| U32(p + 18) != entry.compressed
				|| U32(p + 22) != entry.size)) {
			return Fail(ZipError::Format, "the local sizes disagree: " + entry.name);
		}
		entry.dataStart = offset + kLocalSize + nameLength + extraLength;
		entry.end = entry.dataStart + entry.compressed;
		if (entry.end > directoryOffset) {
			return Fail(ZipError::Format, "an entry runs past the directory");
		}
		if ((entry.flags & 8) != 0) {
			const auto left = directoryOffset - entry.end;
			const auto d = data + entry.end;
			auto fields = d;
			auto length = size_t(12);
			if (left >= 4 && U32(d) == kDescriptorSignature) {
				fields = d + 4;
				length = 16;
			}
			if (left < length
				|| U32(fields) != entry.crc
				|| U32(fields + 4) != entry.compressed
				|| U32(fields + 8) != entry.size) {
				return Fail(ZipError::Format, "a bad data descriptor: " + entry.name);
			}
			entry.end += length;
		}
	}

	auto order = std::vector<const Entry *>();
	for (const auto &entry : list) {
		order.push_back(&entry);
	}
	std::sort(order.begin(), order.end(), [](auto a, auto b) {
		return a->offset < b->offset;
	});
	auto expected = uint64_t(0);
	for (const auto entry : order) {
		if (entry->offset != expected) {
			return Fail(ZipError::Format, "entries overlap or leave gaps");
		}
		expected = entry->end;
	}
	if (expected != directoryOffset) {
		return Fail(ZipError::Format, "unused data before the central directory");
	}

	auto result = std::vector<ZipFile>();
	for (const auto &entry : list) {
		if (entry.directory) {
			continue;
		}
		auto file = ZipFile();
		file.name = entry.name;
		file.data.resize(entry.size);
		const auto input = data + entry.dataStart;
		if (entry.method == 0) {
			std::copy(input, input + entry.size, file.data.begin());
		} else {
			auto stream = z_stream();
			if (inflateInit2(&stream, -15) != Z_OK) {
				return Fail(ZipError::Corrupt, "zlib failed to start");
			}
			stream.next_in = const_cast<Bytef *>(input);
			stream.avail_in = entry.compressed;
			auto scratch = Bytef(0);
			stream.next_out = file.data.empty()
				? &scratch
				: reinterpret_cast<Bytef *>(file.data.data());
			stream.avail_out = file.data.empty() ? 1 : entry.size;
			const auto status = inflate(&stream, Z_FINISH);
			const auto produced = stream.total_out;
			const auto consumed = stream.total_in;
			inflateEnd(&stream);
			if (status != Z_STREAM_END
				|| produced != entry.size
				|| consumed != entry.compressed) {
				return Fail(ZipError::Corrupt, "a damaged compressed entry: " + entry.name);
			}
		}
		if (uint32_t(crc32(0, file.data.data(), uInt(file.data.size()))) != entry.crc) {
			return Fail(ZipError::Corrupt, "a wrong checksum: " + entry.name);
		}
		result.push_back(std::move(file));
	}
	return result;
}

} // namespace fgpl
