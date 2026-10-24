// This is the source code of freshGram.
#include "ayu/utils/strip_metadata.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryFile>
#include <QtGui/QColorSpace>
#include <QtGui/QImage>

#include <algorithm>
#include <array>
#include <cstring>

namespace FreshMetadata {
namespace {

constexpr auto kCopyBlock = qint64(1 << 20);
constexpr auto kMaxBoxDepth = 8;
constexpr auto kTempKeepDays = 1;

[[nodiscard]] uint32 ReadBe32(const uchar *data) {
	return (uint32(data[0]) << 24)
		| (uint32(data[1]) << 16)
		| (uint32(data[2]) << 8)
		| uint32(data[3]);
}

[[nodiscard]] uint32 ReadLe32(const uchar *data) {
	return (uint32(data[3]) << 24)
		| (uint32(data[2]) << 16)
		| (uint32(data[1]) << 8)
		| uint32(data[0]);
}

[[nodiscard]] uint64 ReadBe64(const uchar *data) {
	return (uint64(ReadBe32(data)) << 32) | ReadBe32(data + 4);
}

void AppendBe16(QByteArray &to, uint32 value) {
	to.append(char((value >> 8) & 0xFF));
	to.append(char(value & 0xFF));
}

void AppendBe32(QByteArray &to, uint32 value) {
	AppendBe16(to, value >> 16);
	AppendBe16(to, value & 0xFFFF);
}

void AppendLe32(QByteArray &to, uint32 value) {
	to.append(char(value & 0xFF));
	to.append(char((value >> 8) & 0xFF));
	to.append(char((value >> 16) & 0xFF));
	to.append(char((value >> 24) & 0xFF));
}

[[nodiscard]] const uchar *Bytes(const QByteArray &data) {
	return reinterpret_cast<const uchar*>(data.constData());
}

[[nodiscard]] uint32 Crc32(const uchar *data, qsizetype size) {
	static const auto table = [] {
		auto result = std::array<uint32, 256>();
		for (auto i = 0; i != 256; ++i) {
			auto value = uint32(i);
			for (auto k = 0; k != 8; ++k) {
				value = (value & 1) ? (0xEDB88320U ^ (value >> 1)) : (value >> 1);
			}
			result[i] = value;
		}
		return result;
	}();
	auto crc = uint32(0xFFFFFFFFU);
	for (auto i = qsizetype(0); i != size; ++i) {
		crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	}
	return crc ^ 0xFFFFFFFFU;
}

[[nodiscard]] int OrientationOf(const uchar *tiff, qsizetype size) {
	if (size < 8) {
		return 1;
	}
	const auto little = (tiff[0] == 'I' && tiff[1] == 'I');
	if (!little && !(tiff[0] == 'M' && tiff[1] == 'M')) {
		return 1;
	}
	const auto read16 = [&](qsizetype at) -> uint32 {
		return little
			? (uint32(tiff[at + 1]) << 8) | tiff[at]
			: (uint32(tiff[at]) << 8) | tiff[at + 1];
	};
	const auto read32 = [&](qsizetype at) -> uint32 {
		return little ? ReadLe32(tiff + at) : ReadBe32(tiff + at);
	};
	const auto ifd = qsizetype(read32(4));
	if (ifd < 8 || ifd + 2 > size) {
		return 1;
	}
	const auto count = qsizetype(read16(ifd));
	for (auto i = qsizetype(0); i != count; ++i) {
		const auto entry = ifd + 2 + i * 12;
		if (entry + 12 > size) {
			break;
		}
		if (read16(entry) == 0x0112 && read16(entry + 2) == 3) {
			const auto value = int(read16(entry + 8));
			return (value >= 1 && value <= 8) ? value : 1;
		}
	}
	return 1;
}

// WHY: viewers rotate by the EXIF tag, so a bare orientation is written back.
[[nodiscard]] QByteArray OrientationTiff(int orientation) {
	auto result = QByteArray();
	result.append("MM", 2);
	AppendBe16(result, 42);
	AppendBe32(result, 8);
	AppendBe16(result, 1);
	AppendBe16(result, 0x0112);
	AppendBe16(result, 3);
	AppendBe32(result, 1);
	AppendBe16(result, uint32(orientation));
	AppendBe16(result, 0);
	AppendBe32(result, 0);
	return result;
}

[[nodiscard]] bool StartsWith(
		const uchar *data,
		qsizetype size,
		const char *prefix,
		qsizetype length) {
	return size >= length && memcmp(data, prefix, length) == 0;
}

[[nodiscard]] std::optional<QByteArray> StripJpeg(const QByteArray &input) {
	const auto data = Bytes(input);
	const auto size = input.size();
	auto result = QByteArray();
	result.reserve(size);
	result.append("\xFF\xD8", 2);
	auto orientation = 1;
	auto orientationWritten = false;
	const auto writeOrientation = [&] {
		if (orientationWritten || orientation < 2) {
			return;
		}
		orientationWritten = true;
		const auto tiff = OrientationTiff(orientation);
		result.append("\xFF\xE1", 2);
		AppendBe16(result, uint32(2 + 6 + tiff.size()));
		result.append("Exif\0\0", 6);
		result.append(tiff);
	};
	auto at = qsizetype(2);
	while (at + 2 <= size) {
		if (data[at] != 0xFF) {
			return std::nullopt;
		}
		const auto marker = data[at + 1];
		if (marker == 0xFF) {
			++at;
			continue;
		} else if (marker == 0xD9) {
			writeOrientation();
			result.append("\xFF\xD9", 2);
			return result;
		} else if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
			result.append(input.mid(at, 2));
			at += 2;
			continue;
		} else if (marker == 0x00) {
			return std::nullopt;
		}
		if (at + 4 > size) {
			return std::nullopt;
		}
		const auto length = qsizetype((uint32(data[at + 2]) << 8) | data[at + 3]);
		if (length < 2 || at + 2 + length > size) {
			return std::nullopt;
		}
		const auto payload = data + at + 4;
		const auto payloadSize = length - 2;
		auto keep = true;
		if (marker == 0xE0) {
			keep = StartsWith(payload, payloadSize, "JFIF\0", 5);
		} else if (marker == 0xE1) {
			keep = false;
			if (StartsWith(payload, payloadSize, "Exif\0\0", 6)) {
				orientation = OrientationOf(payload + 6, payloadSize - 6);
			}
		} else if (marker == 0xE2) {
			keep = StartsWith(payload, payloadSize, "ICC_PROFILE\0", 12);
		} else if (marker == 0xEE) {
			keep = StartsWith(payload, payloadSize, "Adobe", 5);
		} else if (marker == 0xFE || (marker >= 0xE3 && marker <= 0xEF)) {
			keep = false;
		}
		if (marker != 0xE0 && marker != 0xE1) {
			writeOrientation();
		}
		if (keep) {
			result.append(input.mid(at, 2 + length));
		}
		at += 2 + length;
		if (marker == 0xDA) {
			while (at + 1 < size) {
				if (data[at] == 0xFF
					&& data[at + 1] != 0x00
					&& !(data[at + 1] >= 0xD0 && data[at + 1] <= 0xD7)
					&& data[at + 1] != 0xFF) {
					break;
				}
				result.append(char(data[at]));
				++at;
			}
		}
	}
	return std::nullopt;
}

[[nodiscard]] bool PngChunkRemoved(const uchar *type) {
	static const char *kRemoved[] = { "tEXt", "zTXt", "iTXt", "eXIf", "tIME" };
	for (const auto name : kRemoved) {
		if (memcmp(type, name, 4) == 0) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] std::optional<QByteArray> StripPng(const QByteArray &input) {
	const auto data = Bytes(input);
	const auto size = input.size();
	auto result = input.left(8);
	auto orientation = 1;
	auto at = qsizetype(8);
	auto header = QByteArray();
	auto body = QByteArray();
	while (at + 12 <= size) {
		const auto length = qsizetype(ReadBe32(data + at));
		if (at + 12 + length > size) {
			return std::nullopt;
		}
		const auto type = data + at + 4;
		const auto whole = input.mid(at, 12 + length);
		at += 12 + length;
		if (memcmp(type, "eXIf", 4) == 0) {
			orientation = OrientationOf(type + 4, length);
		}
		if (!PngChunkRemoved(type)) {
			(memcmp(type, "IHDR", 4) == 0 ? header : body).append(whole);
		}
		if (memcmp(type, "IEND", 4) == 0) {
			result.append(header);
			if (orientation >= 2) {
				const auto tiff = OrientationTiff(orientation);
				auto chunk = QByteArray();
				AppendBe32(chunk, uint32(tiff.size()));
				chunk.append("eXIf", 4);
				chunk.append(tiff);
				AppendBe32(chunk, Crc32(Bytes(chunk) + 4, chunk.size() - 4));
				result.append(chunk);
			}
			result.append(body);
			return result;
		}
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<QByteArray> StripWebp(const QByteArray &input) {
	const auto data = Bytes(input);
	const auto size = input.size();
	if (size < 20) {
		return std::nullopt;
	}
	auto chunks = QByteArray();
	auto orientation = 1;
	auto extended = false;
	auto at = qsizetype(12);
	while (at + 8 <= size) {
		const auto length = qsizetype(ReadLe32(data + at + 4));
		const auto padded = length + (length & 1);
		if (at + 8 + length > size) {
			return std::nullopt;
		}
		const auto type = data + at;
		auto whole = input.mid(at, std::min(8 + padded, size - at));
		if (memcmp(type, "EXIF", 4) == 0) {
			const auto payload = data + at + 8;
			const auto skip = StartsWith(payload, length, "Exif\0\0", 6) ? 6 : 0;
			orientation = OrientationOf(payload + skip, length - skip);
		} else if (memcmp(type, "VP8X", 4) == 0 && length >= 10) {
			extended = true;
			whole[8] = char(uchar(whole[8]) & ~0x0C);
			chunks.append(whole);
		} else if (memcmp(type, "XMP ", 4) != 0) {
			chunks.append(whole);
		}
		at += 8 + padded;
	}
	if (!extended) {
		return input;
	}
	if (orientation >= 2) {
		const auto tiff = OrientationTiff(orientation);
		auto chunk = QByteArray("EXIF", 4);
		AppendLe32(chunk, uint32(tiff.size()));
		chunk.append(tiff);
		chunks.append(chunk);
		const auto flags = chunks.indexOf("VP8X") + 8;
		chunks[flags] = char(uchar(chunks[flags]) | 0x08);
	}
	auto result = QByteArray("RIFF", 4);
	AppendLe32(result, uint32(4 + chunks.size()));
	result.append("WEBP", 4);
	result.append(chunks);
	return result;
}

struct BoxHeader {
	qint64 start = 0;
	qint64 size = 0;
	int header = 0;
	std::array<char, 4> type = {};
};

[[nodiscard]] bool ReadBox(
		QFile &file,
		qint64 at,
		qint64 end,
		BoxHeader &box) {
	if (at + 8 > end || !file.seek(at)) {
		return false;
	}
	auto head = std::array<uchar, 16>();
	if (file.read(reinterpret_cast<char*>(head.data()), 8) != 8) {
		return false;
	}
	box.start = at;
	box.header = 8;
	box.size = ReadBe32(head.data());
	memcpy(box.type.data(), head.data() + 4, 4);
	if (box.size == 1) {
		if (at + 16 > end
			|| file.read(reinterpret_cast<char*>(head.data()) + 8, 8) != 8) {
			return false;
		}
		box.header = 16;
		box.size = qint64(ReadBe64(head.data() + 8));
	} else if (box.size == 0) {
		box.size = end - at;
	}
	return (box.size >= box.header) && (at + box.size <= end);
}

[[nodiscard]] bool Is(const BoxHeader &box, const char *type) {
	return memcmp(box.type.data(), type, 4) == 0;
}

struct Patches {
	std::vector<std::pair<qint64, qint64>> zeros;
	std::vector<qint64> frees;
};

void BlankBox(Patches &patches, const BoxHeader &box) {
	patches.frees.push_back(box.start + 4);
	patches.zeros.emplace_back(box.start + box.header, box.start + box.size);
}

void ZeroTimes(Patches &patches, QFile &file, const BoxHeader &box) {
	auto version = char();
	if (!file.seek(box.start + box.header) || file.read(&version, 1) != 1) {
		return;
	}
	const auto width = (version == 1) ? 8 : 4;
	const auto from = box.start + box.header + 4;
	if (box.size >= box.header + 4 + 2 * width) {
		patches.zeros.emplace_back(from, from + 2 * width);
	}
}

[[nodiscard]] bool Walk(
		QFile &file,
		qint64 begin,
		qint64 end,
		int depth,
		Patches &patches,
		bool &foundMoov) {
	if (depth > kMaxBoxDepth) {
		return false;
	}
	auto at = begin;
	while (at + 8 <= end) {
		auto box = BoxHeader();
		if (!ReadBox(file, at, end, box)) {
			return false;
		}
		const auto next = at + box.size;
		const auto inner = box.start + box.header;
		if (Is(box, "udta") || Is(box, "meta") || Is(box, "XMP_")) {
			BlankBox(patches, box);
		} else if (depth == 0 && Is(box, "uuid")) {
			BlankBox(patches, box);
		} else if (Is(box, "mvhd") || Is(box, "tkhd") || Is(box, "mdhd")) {
			ZeroTimes(patches, file, box);
		} else if (Is(box, "moov")) {
			foundMoov = true;
			if (!Walk(file, inner, next, depth + 1, patches, foundMoov)) {
				return false;
			}
		} else if (depth > 0 && (Is(box, "trak") || Is(box, "mdia"))) {
			if (!Walk(file, inner, next, depth + 1, patches, foundMoov)) {
				return false;
			}
		}
		at = next;
	}
	return true;
}

[[nodiscard]] bool CopyPatched(
		QFile &source,
		QFile &target,
		Patches &patches) {
	std::sort(patches.zeros.begin(), patches.zeros.end());
	std::sort(patches.frees.begin(), patches.frees.end());
	if (!source.seek(0)) {
		return false;
	}
	auto buffer = QByteArray(kCopyBlock, Qt::Uninitialized);
	auto position = qint64(0);
	while (true) {
		const auto read = source.read(buffer.data(), kCopyBlock);
		if (read < 0) {
			return false;
		} else if (!read) {
			break;
		}
		const auto till = position + read;
		for (const auto &[from, to] : patches.zeros) {
			const auto a = std::max(from, position);
			const auto b = std::min(to, till);
			if (a < b) {
				memset(buffer.data() + (a - position), 0, size_t(b - a));
			}
		}
		for (const auto offset : patches.frees) {
			for (auto i = qint64(0); i != 4; ++i) {
				if (offset + i >= position && offset + i < till) {
					buffer[int(offset + i - position)] = "free"[i];
				}
			}
		}
		if (target.write(buffer.constData(), read) != read) {
			return false;
		}
		position = till;
	}
	return true;
}

} // namespace

MediaKind KindOf(const QString &path, const QString &mime) {
	const auto extension = QFileInfo(path).suffix().toLower();
	if (extension == u"jpg"_q
		|| extension == u"jpeg"_q
		|| extension == u"png"_q
		|| extension == u"webp"_q
		|| mime == u"image/jpeg"_q
		|| mime == u"image/png"_q
		|| mime == u"image/webp"_q) {
		return MediaKind::Image;
	} else if (extension == u"mp4"_q
		|| extension == u"mov"_q
		|| extension == u"m4v"_q
		|| mime == u"video/mp4"_q
		|| mime == u"video/quicktime"_q) {
		return MediaKind::Video;
	}
	return MediaKind::None;
}

QString NewTempVideoPath(const QString &extension) {
	const auto directory = QDir::tempPath() + u"/tdclean"_q;
	QDir().mkpath(directory);
	QFile::setPermissions(
		directory,
		QFileDevice::ReadUser
			| QFileDevice::WriteUser
			| QFileDevice::ExeUser);
	const auto border = QDateTime::currentDateTime().addDays(-kTempKeepDays);
	for (const auto &old : QDir(directory).entryInfoList(QDir::Files)) {
		if (old.lastModified() < border) {
			QFile::remove(old.absoluteFilePath());
		}
	}
	auto file = QTemporaryFile(directory + u"/XXXXXX."_q + extension);
	file.setAutoRemove(false);
	return file.open() ? file.fileName() : QString();
}

std::optional<QByteArray> StripImage(const QByteArray &bytes) {
	if (bytes.size() > 12
		&& uchar(bytes[0]) == 0xFF
		&& uchar(bytes[1]) == 0xD8) {
		return StripJpeg(bytes);
	} else if (bytes.startsWith("\x89PNG\r\n\x1A\n")) {
		return StripPng(bytes);
	} else if (bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP") {
		return StripWebp(bytes);
	}
	return std::nullopt;
}

// WHY: a pasted image has no bytes, its text chunks live in QImage::text().
QImage StripImageText(const QImage &image) {
	if (image.isNull() || image.textKeys().isEmpty()) {
		return image;
	}
	auto result = QImage(
		image.constBits(),
		image.width(),
		image.height(),
		image.bytesPerLine(),
		image.format()).copy();
	result.setColorTable(image.colorTable());
	result.setColorSpace(image.colorSpace());
	result.setDevicePixelRatio(image.devicePixelRatio());
	return result;
}

bool StripVideo(const QString &source, const QString &target) {
	auto from = QFile(source);
	if (!from.open(QIODevice::ReadOnly)) {
		return false;
	}
	auto patches = Patches();
	auto foundMoov = false;
	if (!Walk(from, 0, from.size(), 0, patches, foundMoov) || !foundMoov) {
		return false;
	}
	auto to = QFile(target);
	if (!to.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}
	const auto ok = CopyPatched(from, to, patches) && to.flush();
	to.close();
	if (!ok) {
		QFile::remove(target);
	}
	return ok;
}

} // namespace FreshMetadata
