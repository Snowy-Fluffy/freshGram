#include "zip_writer.h"

#include <zlib.h>

namespace fgpl {
namespace {

void Put16(cbor::Bytes &out, uint16_t value) {
	out.push_back(uint8_t(value));
	out.push_back(uint8_t(value >> 8));
}

void Put32(cbor::Bytes &out, uint32_t value) {
	for (auto i = 0; i != 4; ++i) {
		out.push_back(uint8_t(value >> (8 * i)));
	}
}

[[nodiscard]] cbor::Bytes Deflate(const cbor::Bytes &data) {
	auto stream = z_stream();
	deflateInit2(&stream, 9, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY);
	auto out = cbor::Bytes(deflateBound(&stream, uLong(data.size())) + 16);
	stream.next_in = const_cast<Bytef *>(data.data());
	stream.avail_in = uInt(data.size());
	stream.next_out = out.data();
	stream.avail_out = uInt(out.size());
	deflate(&stream, Z_FINISH);
	out.resize(stream.total_out);
	deflateEnd(&stream);
	return out;
}

} // namespace

cbor::Bytes WriteZip(
		const std::vector<std::pair<std::string, cbor::Bytes>> &files) {
	constexpr auto kDosTime = uint16_t(0);
	constexpr auto kDosDate = uint16_t(0x21);
	auto out = cbor::Bytes();
	auto central = cbor::Bytes();
	for (const auto &file : files) {
		const auto offset = uint32_t(out.size());
		const auto packed = Deflate(file.second);
		const auto deflated = packed.size() < file.second.size();
		const auto &stored = deflated ? packed : file.second;
		const auto method = uint16_t(deflated ? 8 : 0);
		const auto crc = uint32_t(crc32(
			0,
			file.second.data(),
			uInt(file.second.size())));
		Put32(out, 0x04034b50);
		Put16(out, 20);
		Put16(out, 0);
		Put16(out, method);
		Put16(out, kDosTime);
		Put16(out, kDosDate);
		Put32(out, crc);
		Put32(out, uint32_t(stored.size()));
		Put32(out, uint32_t(file.second.size()));
		Put16(out, uint16_t(file.first.size()));
		Put16(out, 0);
		out.insert(out.end(), file.first.begin(), file.first.end());
		out.insert(out.end(), stored.begin(), stored.end());

		Put32(central, 0x02014b50);
		Put16(central, 0x031E);
		Put16(central, 20);
		Put16(central, 0);
		Put16(central, method);
		Put16(central, kDosTime);
		Put16(central, kDosDate);
		Put32(central, crc);
		Put32(central, uint32_t(stored.size()));
		Put32(central, uint32_t(file.second.size()));
		Put16(central, uint16_t(file.first.size()));
		Put16(central, 0);
		Put16(central, 0);
		Put16(central, 0);
		Put16(central, 0);
		Put32(central, 0x81A40000);
		Put32(central, offset);
		central.insert(central.end(), file.first.begin(), file.first.end());
	}
	const auto directoryOffset = uint32_t(out.size());
	out.insert(out.end(), central.begin(), central.end());
	Put32(out, 0x06054b50);
	Put16(out, 0);
	Put16(out, 0);
	Put16(out, uint16_t(files.size()));
	Put16(out, uint16_t(files.size()));
	Put32(out, uint32_t(central.size()));
	Put32(out, directoryOffset);
	Put16(out, 0);
	return out;
}

} // namespace fgpl
