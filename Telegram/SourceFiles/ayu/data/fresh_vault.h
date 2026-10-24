// This file is part of freshGram.
#pragma once

#include <cstddef>
#include <memory>
#include <optional>

namespace MTP {
class AuthKey;
using AuthKeyPtr = std::shared_ptr<AuthKey>;
} // namespace MTP

namespace FreshVault {

void SetKey(MTP::AuthKeyPtr key);
[[nodiscard]] bool HasKey();
// A key of 32 bytes for one purpose, made from the local key.
[[nodiscard]] bool DeriveKey(const char *label, unsigned char *out, size_t size);

[[nodiscard]] bool IsSealed(const QByteArray &bytes);
[[nodiscard]] QByteArray Seal(const QByteArray &plain);
[[nodiscard]] bool Unseal(
	const QByteArray &sealed,
	QByteArray &plain,
	bool requireSealed = false);

// Fails without the key, nothing is ever written open.
bool WriteFile(const QString &path, const QByteArray &plain);
// Overwrites the file with zeros before removing it.
bool WipeFile(const QString &path);
bool WriteSealedFile(const QString &path, const QByteArray &plain);
// Packs the data by blocks and seals every block, only one block is held at a time.
bool WriteCompressedFile(const QString &path, const unsigned char *data, qint64 size);
[[nodiscard]] std::optional<QByteArray> ReadFile(
	const QString &path,
	bool requireSealed = false);
void SealFilesIn(const QString &directory);

} // namespace FreshVault
