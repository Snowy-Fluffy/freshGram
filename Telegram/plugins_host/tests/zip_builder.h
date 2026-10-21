#pragma once

#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace zipbuild {

struct Item {
	std::string name;
	std::vector<uint8_t> data;
	bool deflate = true;
	uint32_t externalAttributes = 0x81A40000;
	uint16_t madeBy = 0x031E;
	uint16_t flags = 0;
	bool descriptor = false;
	bool directory = false;
	int crcDelta = 0;
	bool breakLocalName = false;
};

inline void Put16(std::vector<uint8_t> &out, uint16_t value) {
	out.push_back(uint8_t(value));
	out.push_back(uint8_t(value >> 8));
}

inline void Put32(std::vector<uint8_t> &out, uint32_t value) {
	for (auto i = 0; i != 4; ++i) {
		out.push_back(uint8_t(value >> (8 * i)));
	}
}

[[nodiscard]] inline std::vector<uint8_t> Compress(
		const std::vector<uint8_t> &data) {
	auto stream = z_stream();
	deflateInit2(&stream, 9, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY);
	auto out = std::vector<uint8_t>(deflateBound(&stream, uLong(data.size())) + 16);
	stream.next_in = const_cast<Bytef *>(data.data());
	stream.avail_in = uInt(data.size());
	stream.next_out = out.data();
	stream.avail_out = uInt(out.size());
	deflate(&stream, Z_FINISH);
	out.resize(stream.total_out);
	deflateEnd(&stream);
	return out;
}

[[nodiscard]] inline std::vector<uint8_t> Build(
		const std::vector<Item> &items,
		size_t trailingBytes = 0) {
	auto out = std::vector<uint8_t>();
	auto central = std::vector<uint8_t>();
	for (const auto &item : items) {
		const auto offset = uint32_t(out.size());
		const auto name = item.directory && item.name.back() != '/'
			? item.name + "/"
			: item.name;
		const auto method = uint16_t((item.deflate && !item.directory) ? 8 : 0);
		const auto stored = method == 8 ? Compress(item.data) : item.data;
		const auto crc = uint32_t(crc32(0, item.data.data(), uInt(item.data.size())))
			+ uint32_t(item.crcDelta);
		const auto flags = uint16_t(item.flags | (item.descriptor ? 8 : 0));
		Put32(out, 0x04034b50);
		Put16(out, 20);
		Put16(out, flags);
		Put16(out, method);
		Put16(out, 0);
		Put16(out, 0x21);
		Put32(out, item.descriptor ? 0 : crc);
		Put32(out, item.descriptor ? 0 : uint32_t(stored.size()));
		Put32(out, item.descriptor ? 0 : uint32_t(item.data.size()));
		Put16(out, uint16_t(name.size()));
		Put16(out, 0);
		auto localName = name;
		if (item.breakLocalName) {
			localName[0] = 'X';
		}
		out.insert(out.end(), localName.begin(), localName.end());
		out.insert(out.end(), stored.begin(), stored.end());
		if (item.descriptor) {
			Put32(out, 0x08074b50);
			Put32(out, crc);
			Put32(out, uint32_t(stored.size()));
			Put32(out, uint32_t(item.data.size()));
		}
		Put32(central, 0x02014b50);
		Put16(central, item.madeBy);
		Put16(central, 20);
		Put16(central, flags);
		Put16(central, method);
		Put16(central, 0);
		Put16(central, 0x21);
		Put32(central, crc);
		Put32(central, uint32_t(stored.size()));
		Put32(central, uint32_t(item.data.size()));
		Put16(central, uint16_t(name.size()));
		Put16(central, 0);
		Put16(central, 0);
		Put16(central, 0);
		Put16(central, 0);
		Put32(central, item.directory ? 0x41ED0000 : item.externalAttributes);
		Put32(central, offset);
		central.insert(central.end(), name.begin(), name.end());
	}
	const auto directoryOffset = uint32_t(out.size());
	out.insert(out.end(), central.begin(), central.end());
	Put32(out, 0x06054b50);
	Put16(out, 0);
	Put16(out, 0);
	Put16(out, uint16_t(items.size()));
	Put16(out, uint16_t(items.size()));
	Put32(out, uint32_t(central.size()));
	Put32(out, directoryOffset);
	Put16(out, 0);
	out.insert(out.end(), trailingBytes, 0);
	return out;
}

} // namespace zipbuild
