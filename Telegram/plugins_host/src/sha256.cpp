#include "sha256.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace fgpl {
namespace {

constexpr uint32_t kRound[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

[[nodiscard]] constexpr uint32_t Rotate(uint32_t value, int bits) {
	return (value >> bits) | (value << (32 - bits));
}

class Hasher {
public:
	void update(const uint8_t *data, size_t size) {
		_total += size;
		while (size != 0) {
			const auto take = std::min(size, sizeof(_block) - _filled);
			std::memcpy(_block + _filled, data, take);
			_filled += take;
			data += take;
			size -= take;
			if (_filled == sizeof(_block)) {
				compress(_block);
				_filled = 0;
			}
		}
	}

	[[nodiscard]] Digest finish() {
		const auto bits = _total * 8;
		const uint8_t one = 0x80;
		update(&one, 1);
		const uint8_t zero = 0;
		while (_filled != 56) {
			update(&zero, 1);
		}
		uint8_t length[8];
		for (auto i = 0; i != 8; ++i) {
			length[i] = uint8_t(bits >> (56 - 8 * i));
		}
		update(length, 8);
		auto result = Digest();
		for (auto i = 0; i != 8; ++i) {
			for (auto k = 0; k != 4; ++k) {
				result[size_t(i * 4 + k)] = uint8_t(_state[i] >> (24 - 8 * k));
			}
		}
		return result;
	}

private:
	void compress(const uint8_t *block) {
		uint32_t w[64];
		for (auto i = 0; i != 16; ++i) {
			w[i] = (uint32_t(block[i * 4]) << 24)
				| (uint32_t(block[i * 4 + 1]) << 16)
				| (uint32_t(block[i * 4 + 2]) << 8)
				| uint32_t(block[i * 4 + 3]);
		}
		for (auto i = 16; i != 64; ++i) {
			const auto s0 = Rotate(w[i - 15], 7)
				^ Rotate(w[i - 15], 18)
				^ (w[i - 15] >> 3);
			const auto s1 = Rotate(w[i - 2], 17)
				^ Rotate(w[i - 2], 19)
				^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		auto a = _state[0], b = _state[1], c = _state[2], d = _state[3];
		auto e = _state[4], f = _state[5], g = _state[6], h = _state[7];
		for (auto i = 0; i != 64; ++i) {
			const auto s1 = Rotate(e, 6) ^ Rotate(e, 11) ^ Rotate(e, 25);
			const auto choose = (e & f) ^ (~e & g);
			const auto first = h + s1 + choose + kRound[i] + w[i];
			const auto s0 = Rotate(a, 2) ^ Rotate(a, 13) ^ Rotate(a, 22);
			const auto majority = (a & b) ^ (a & c) ^ (b & c);
			const auto second = s0 + majority;
			h = g;
			g = f;
			f = e;
			e = d + first;
			d = c;
			c = b;
			b = a;
			a = first + second;
		}
		_state[0] += a;
		_state[1] += b;
		_state[2] += c;
		_state[3] += d;
		_state[4] += e;
		_state[5] += f;
		_state[6] += g;
		_state[7] += h;
	}

	uint32_t _state[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};
	uint8_t _block[64] = {};
	size_t _filled = 0;
	uint64_t _total = 0;

};

} // namespace

Digest Sha256(std::span<const uint8_t> data) {
	auto hasher = Hasher();
	hasher.update(data.data(), data.size());
	return hasher.finish();
}

Digest HmacSha256(
		std::span<const uint8_t> key,
		std::span<const uint8_t> data) {
	constexpr auto kBlock = size_t(64);
	uint8_t padded[kBlock] = {};
	if (key.size() > kBlock) {
		const auto digest = Sha256(key);
		std::memcpy(padded, digest.data(), digest.size());
	} else if (!key.empty()) {
		std::memcpy(padded, key.data(), key.size());
	}
	uint8_t inner[kBlock];
	uint8_t outer[kBlock];
	for (auto i = size_t(0); i != kBlock; ++i) {
		inner[i] = padded[i] ^ 0x36;
		outer[i] = padded[i] ^ 0x5c;
	}
	auto first = Hasher();
	first.update(inner, kBlock);
	first.update(data.data(), data.size());
	const auto middle = first.finish();
	auto second = Hasher();
	second.update(outer, kBlock);
	second.update(middle.data(), middle.size());
	return second.finish();
}

bool ConstantTimeEqual(
		std::span<const uint8_t> a,
		std::span<const uint8_t> b) {
	if (a.size() != b.size()) {
		return false;
	}
	auto difference = uint8_t(0);
	for (auto i = size_t(0); i != a.size(); ++i) {
		difference |= uint8_t(a[i] ^ b[i]);
	}
	return difference == 0;
}

} // namespace fgpl
