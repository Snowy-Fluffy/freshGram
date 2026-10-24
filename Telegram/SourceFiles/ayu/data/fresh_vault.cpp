// This file is part of freshGram.
#include "ayu/data/fresh_vault.h"

#include "mtproto/mtproto_auth_key.h"
#include "storage/details/storage_file_utilities.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSaveFile>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace FreshVault {
namespace {

using namespace Storage::details;

constexpr auto kMagic = "AYV1";
constexpr auto kCompressedMagic = "AYV2";
constexpr auto kMagicSize = 4;
constexpr auto kBlockSize = qint64(1) << 20;

std::mutex KeyMutex;
MTP::AuthKeyPtr Key;

[[nodiscard]] MTP::AuthKeyPtr GetKey() {
	const auto lock = std::lock_guard(KeyMutex);
	return Key;
}

} // namespace

void SetKey(MTP::AuthKeyPtr key) {
	const auto lock = std::lock_guard(KeyMutex);
	Key = std::move(key);
}

bool HasKey() {
	return GetKey() != nullptr;
}

bool DeriveKey(const char *label, unsigned char *out, size_t size) {
	const auto key = GetKey();
	if (!key || size != 32) {
		return false;
	}
	const auto data = key->data();
	auto length = 0U;
	return HMAC(
		EVP_sha256(),
		data.data(),
		int(data.size()),
		reinterpret_cast<const unsigned char*>(label),
		std::strlen(label),
		out,
		&length) != nullptr && length == 32;
}

bool IsSealed(const QByteArray &bytes) {
	return bytes.size() > kMagicSize
		&& !memcmp(bytes.constData(), kMagic, kMagicSize);
}

QByteArray Seal(const QByteArray &plain) {
	const auto key = GetKey();
	if (!key) {
		// WHY: nothing is written open, the caller sees the failure.
		return QByteArray();
	}
	auto descriptor = EncryptedDescriptor(plain.size() + sizeof(quint32));
	descriptor.stream << plain;
	const auto encrypted = PrepareEncrypted(descriptor, key);
	auto result = QByteArray();
	result.reserve(kMagicSize + encrypted.size());
	result.append(kMagic, kMagicSize);
	result.append(encrypted);
	return result;
}

bool Unseal(
		const QByteArray &sealed,
		QByteArray &plain,
		bool requireSealed) {
	if (!IsSealed(sealed)) {
		if (requireSealed) {
			return false;
		}
		plain = sealed;
		return true;
	}
	const auto key = GetKey();
	if (!key) {
		return false;
	}
	auto descriptor = EncryptedDescriptor();
	if (!DecryptLocal(descriptor, sealed.mid(kMagicSize), key)) {
		return false;
	}
	auto result = QByteArray();
	descriptor.stream >> result;
	if (descriptor.stream.status() != QDataStream::Ok) {
		return false;
	}
	plain = std::move(result);
	return true;
}

bool WriteFile(const QString &path, const QByteArray &plain) {
	const auto bytes = Seal(plain);
	if (bytes.isEmpty()) {
		return false;
	}
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	return (file.write(bytes) == bytes.size()) && file.commit();
}

bool WipeFile(const QString &path) {
	auto file = QFile(path);
	if (file.open(QIODevice::ReadWrite)) {
		constexpr auto kChunk = qint64(64 * 1024);
		const auto zeros = QByteArray(int(kChunk), '\0');
		for (auto left = file.size(); left > 0;) {
			const auto chunk = std::min(left, kChunk);
			if (file.write(zeros.constData(), chunk) != chunk) {
				break;
			}
			left -= chunk;
		}
		file.flush();
		file.close();
	}
	return QFile::remove(path) || !QFile::exists(path);
}

bool WriteSealedFile(const QString &path, const QByteArray &plain) {
	return HasKey() && WriteFile(path, plain);
}

namespace {

[[nodiscard]] bool IsCompressed(const QByteArray &bytes) {
	return bytes.size() >= kMagicSize
		&& !memcmp(bytes.constData(), kCompressedMagic, kMagicSize);
}

void WriteNumber(QByteArray &to, quint64 value, int size) {
	for (auto i = size - 1; i >= 0; --i) {
		to.append(char((value >> (8 * i)) & 0xFF));
	}
}

[[nodiscard]] bool ReadNumber(const QByteArray &from, qint64 &position, int size, quint64 &value) {
	if (from.size() - position < size) {
		return false;
	}
	value = 0;
	for (auto i = 0; i != size; ++i) {
		value = (value << 8) | quint8(from.constData()[position + i]);
	}
	position += size;
	return true;
}

[[nodiscard]] bool ReadCompressed(const QByteArray &bytes, QByteArray &plain) {
	auto position = qint64(kMagicSize);
	plain.clear();
	while (true) {
		auto length = quint64();
		if (!ReadNumber(bytes, position, 4, length)) {
			return false;
		}
		if (!length) {
			break;
		}
		if (qint64(length) > bytes.size() - position) {
			return false;
		}
		const auto block = QByteArray::fromRawData(
			bytes.constData() + position,
			int(length));
		position += qint64(length);
		auto packed = QByteArray();
		if (!Unseal(block, packed, true)) {
			return false;
		}
		const auto unpacked = qUncompress(packed);
		if (unpacked.isEmpty()) {
			return false;
		}
		plain.append(unpacked);
	}
	auto expected = quint64();
	return ReadNumber(bytes, position, 8, expected)
		&& qint64(expected) == plain.size();
}

} // namespace

bool WriteCompressedFile(const QString &path, const unsigned char *data, qint64 size) {
	if (!HasKey()) {
		return false;
	}
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)
		|| file.write(kCompressedMagic, kMagicSize) != kMagicSize) {
		return false;
	}
	for (auto offset = qint64(0); offset < size; offset += kBlockSize) {
		const auto length = std::min(kBlockSize, size - offset);
		const auto sealed = Seal(qCompress(data + offset, int(length), 3));
		if (sealed.isEmpty()) {
			return false;
		}
		auto head = QByteArray();
		WriteNumber(head, quint64(sealed.size()), 4);
		if (file.write(head) != head.size() || file.write(sealed) != sealed.size()) {
			return false;
		}
	}
	auto tail = QByteArray();
	WriteNumber(tail, 0, 4);
	WriteNumber(tail, quint64(size), 8);
	return file.write(tail) == tail.size() && file.commit();
}

std::optional<QByteArray> ReadFile(const QString &path, bool requireSealed) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return std::nullopt;
	}
	const auto bytes = file.readAll();
	auto plain = QByteArray();
	if (IsCompressed(bytes)) {
		if (!HasKey() || !ReadCompressed(bytes, plain)) {
			return std::nullopt;
		}
		return plain;
	}
	if (!Unseal(bytes, plain, requireSealed)) {
		return std::nullopt;
	}
	return plain;
}

void SealFilesIn(const QString &directory) {
	if (!HasKey()) {
		return;
	}
	const auto dir = QDir(directory);
	for (const auto &info : dir.entryInfoList(QDir::Files)) {
		auto head = QByteArray();
		{
			auto file = QFile(info.absoluteFilePath());
			if (!file.open(QIODevice::ReadOnly)) {
				continue;
			}
			head = file.read(kMagicSize + 1);
		}
		if (IsSealed(head)) {
			continue;
		}
		if (const auto plain = ReadFile(info.absoluteFilePath())) {
			WriteFile(info.absoluteFilePath(), *plain);
		}
	}
}

} // namespace FreshVault
