#pragma once

#include "fgpl/base64.h"
#include "fgpl/cbor.h"

#include <array>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fgpl::ws {

constexpr auto kMaxMessage = size_t(1) << 20;
constexpr auto kMaxControl = size_t(125);

constexpr auto kOpContinue = uint8_t(0x0);
constexpr auto kOpText = uint8_t(0x1);
constexpr auto kOpBinary = uint8_t(0x2);
constexpr auto kOpClose = uint8_t(0x8);
constexpr auto kOpPing = uint8_t(0x9);
constexpr auto kOpPong = uint8_t(0xA);

[[nodiscard]] inline std::array<uint8_t, 20> Sha1(std::string_view data) {
	auto h = std::array<uint32_t, 5>{
		0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U };
	auto message = std::vector<uint8_t>(data.begin(), data.end());
	const auto bits = uint64_t(data.size()) * 8;
	message.push_back(0x80);
	while (message.size() % 64 != 56) {
		message.push_back(0);
	}
	for (auto i = 7; i >= 0; --i) {
		message.push_back(uint8_t(bits >> (i * 8)));
	}
	const auto rotl = [](uint32_t value, int count) {
		return (value << count) | (value >> (32 - count));
	};
	for (auto offset = size_t(0); offset != message.size(); offset += 64) {
		auto w = std::array<uint32_t, 80>();
		for (auto i = 0; i != 16; ++i) {
			const auto p = &message[offset + 4 * i];
			w[i] = (uint32_t(p[0]) << 24)
				| (uint32_t(p[1]) << 16)
				| (uint32_t(p[2]) << 8)
				| uint32_t(p[3]);
		}
		for (auto i = 16; i != 80; ++i) {
			w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		}
		auto a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (auto i = 0; i != 80; ++i) {
			auto f = uint32_t();
			auto k = uint32_t();
			if (i < 20) {
				f = (b & c) | (~b & d);
				k = 0x5A827999U;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ED9EBA1U;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8F1BBCDCU;
			} else {
				f = b ^ c ^ d;
				k = 0xCA62C1D6U;
			}
			const auto next = rotl(a, 5) + f + e + k + w[i];
			e = d;
			d = c;
			c = rotl(b, 30);
			b = a;
			a = next;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
	}
	auto result = std::array<uint8_t, 20>();
	for (auto i = 0; i != 5; ++i) {
		for (auto j = 0; j != 4; ++j) {
			result[i * 4 + j] = uint8_t(h[i] >> (24 - 8 * j));
		}
	}
	return result;
}

[[nodiscard]] inline std::string AcceptFor(std::string_view key) {
	const auto hash = Sha1(std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
	return Base64Encode(hash);
}

[[nodiscard]] inline std::string NewKey() {
	auto random = std::random_device();
	auto bytes = std::array<uint8_t, 16>();
	for (auto &byte : bytes) {
		byte = uint8_t(random());
	}
	return Base64Encode(bytes);
}

[[nodiscard]] inline bool IsValidUtf8(std::span<const uint8_t> data) {
	auto i = size_t(0);
	while (i != data.size()) {
		const auto lead = data[i];
		auto length = size_t(0);
		auto minimum = uint32_t(0);
		auto code = uint32_t(0);
		if (lead < 0x80) {
			++i;
			continue;
		} else if ((lead & 0xE0) == 0xC0) {
			length = 2;
			minimum = 0x80;
			code = lead & 0x1F;
		} else if ((lead & 0xF0) == 0xE0) {
			length = 3;
			minimum = 0x800;
			code = lead & 0x0F;
		} else if ((lead & 0xF8) == 0xF0) {
			length = 4;
			minimum = 0x10000;
			code = lead & 0x07;
		} else {
			return false;
		}
		if (i + length > data.size()) {
			return false;
		}
		for (auto j = size_t(1); j != length; ++j) {
			if ((data[i + j] & 0xC0) != 0x80) {
				return false;
			}
			code = (code << 6) | (data[i + j] & 0x3F);
		}
		if (code < minimum
			|| code > 0x10FFFF
			|| (code >= 0xD800 && code <= 0xDFFF)) {
			return false;
		}
		i += length;
	}
	return true;
}

// A client frame: always final, always masked.
[[nodiscard]] inline cbor::Bytes EncodeFrame(
		uint8_t opcode,
		std::span<const uint8_t> payload) {
	auto random = std::random_device();
	auto result = cbor::Bytes();
	result.push_back(uint8_t(0x80 | opcode));
	if (payload.size() < 126) {
		result.push_back(uint8_t(0x80 | payload.size()));
	} else if (payload.size() <= 0xFFFF) {
		result.push_back(0x80 | 126);
		result.push_back(uint8_t(payload.size() >> 8));
		result.push_back(uint8_t(payload.size()));
	} else {
		result.push_back(0x80 | 127);
		for (auto i = 7; i >= 0; --i) {
			result.push_back(uint8_t(uint64_t(payload.size()) >> (i * 8)));
		}
	}
	auto mask = std::array<uint8_t, 4>();
	for (auto &byte : mask) {
		byte = uint8_t(random());
		result.push_back(byte);
	}
	for (auto i = size_t(0); i != payload.size(); ++i) {
		result.push_back(payload[i] ^ mask[i % 4]);
	}
	return result;
}

[[nodiscard]] inline cbor::Bytes CloseFrame(int code, std::string_view reason) {
	auto payload = cbor::Bytes();
	if (code > 0) {
		payload.push_back(uint8_t(code >> 8));
		payload.push_back(uint8_t(code));
		payload.insert(payload.end(), reason.begin(), reason.end());
	}
	return EncodeFrame(kOpClose, payload);
}

struct Frame {
	bool fin = false;
	uint8_t opcode = 0;
	cbor::Bytes payload;
};

// Server frames: unmasked, no extensions, at most kMaxMessage per frame.
class Parser {
public:
	enum class Status {
		Frame,
		More,
		Error,
	};

	void feed(std::span<const uint8_t> data) {
		_buffer.insert(_buffer.end(), data.begin(), data.end());
	}

	[[nodiscard]] Status next(Frame &frame) {
		const auto available = _buffer.size() - _offset;
		if (available < 2) {
			return Status::More;
		}
		const auto p = _buffer.data() + _offset;
		if ((p[0] & 0x70) || (p[1] & 0x80)) {
			return Status::Error;
		}
		const auto opcode = uint8_t(p[0] & 0x0F);
		const auto fin = (p[0] & 0x80) != 0;
		auto length = uint64_t(p[1] & 0x7F);
		auto header = size_t(2);
		if (length == 126) {
			if (available < 4) {
				return Status::More;
			}
			length = (uint64_t(p[2]) << 8) | p[3];
			header = 4;
		} else if (length == 127) {
			if (available < 10) {
				return Status::More;
			}
			length = 0;
			for (auto i = 0; i != 8; ++i) {
				length = (length << 8) | p[2 + i];
			}
			header = 10;
		}
		const auto control = (opcode & 0x8) != 0;
		const auto known = opcode <= kOpBinary
			|| opcode == kOpClose
			|| opcode == kOpPing
			|| opcode == kOpPong;
		if (!known
			|| (control && (!fin || length > kMaxControl))
			|| length > kMaxMessage) {
			return Status::Error;
		} else if (available < header + length) {
			return Status::More;
		}
		frame.fin = fin;
		frame.opcode = opcode;
		frame.payload.assign(p + header, p + header + length);
		_offset += header + size_t(length);
		if (_offset == _buffer.size()) {
			_buffer.clear();
			_offset = 0;
		} else if (_offset > (size_t(64) << 10)) {
			_buffer.erase(_buffer.begin(), _buffer.begin() + _offset);
			_offset = 0;
		}
		return Status::Frame;
	}

private:
	cbor::Bytes _buffer;
	size_t _offset = 0;

};

} // namespace fgpl::ws
