#pragma once

#include <cstdint>
#include <vector>

namespace FreshSecret {

using Bytes = std::vector<uint8_t>;

constexpr auto kKeySize = 256;

void RandomBytes(uint8_t *data, size_t size);
[[nodiscard]] Bytes RandomVector(size_t size);

[[nodiscard]] int64_t KeyFingerprint(const Bytes &key);
[[nodiscard]] Bytes PadKey(const Bytes &key);
[[nodiscard]] Bytes KeyVisualHash(const Bytes &key);
void UpdateKeyVisualHash(Bytes &hash, const Bytes &key, int layer);

[[nodiscard]] Bytes EncryptPacket(
	const Bytes &key,
	bool creator,
	const Bytes &object);

[[nodiscard]] bool DecryptPacket(
	const Bytes &key,
	bool creator,
	const Bytes &packet,
	Bytes &object);

} // namespace FreshSecret
