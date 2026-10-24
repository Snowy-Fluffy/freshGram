// This is the source code of freshGram.
#include "ayu/data/fresh_encrypted_vfs.h"

#include "ayu/libs/sqlite/sqlite3.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace FreshEncryptedVfs {
namespace {

constexpr auto kHeaderSize = 4096;
constexpr auto kNonceSize = 12;
constexpr auto kTagSize = 16;
constexpr auto kContentSize = kPageSize - kReserveBytes;
// The journal header: magic, nonce and the HMAC of the content at the last sync.
constexpr auto kJournalHeaderSize = 64;
constexpr auto kJournalNonceOffset = 8;
constexpr auto kJournalMacOffset = 24;
constexpr auto kJournalMacSize = 32;
constexpr auto kJournalReadChunk = 64 * 1024;
constexpr char kMainMagic[8] = {'A', 'Y', 'U', 'E', 'N', 'C', '0', '1'};
constexpr char kJournalMagic[8] = {'A', 'Y', 'U', 'J', 'R', 'N', '0', '2'};

enum class Kind {
	Plain,
	Main,
	Journal,
	Temp,
};

using Key = std::array<unsigned char, 32>;

struct File {
	sqlite3_file base;
	sqlite3_file *real = nullptr;
	Kind kind = Kind::Plain;
	Key key = {};
	std::array<unsigned char, 16> salt = {};
	Key macKey = {};
	bool headerReady = false;
	bool written = false;
	bool verify = false;
	bool rejected = false;
};

std::mutex StateMutex;
sqlite3_vfs Vfs = {};
sqlite3_vfs *Base = nullptr;
Key MasterKey = {};
std::string Prefix;
bool Registered = false;

[[nodiscard]] File *Self(sqlite3_file *file) {
	return reinterpret_cast<File*>(file);
}

[[nodiscard]] Key Derive(const char *label, const unsigned char *extra, size_t extraSize) {
	auto data = std::vector<unsigned char>(label, label + std::strlen(label));
	data.insert(data.end(), extra, extra + extraSize);
	auto result = Key();
	auto size = 0U;
	HMAC(
		EVP_sha256(),
		MasterKey.data(),
		int(MasterKey.size()),
		data.data(),
		data.size(),
		result.data(),
		&size);
	return result;
}

void RandomBytes(unsigned char *data, size_t size) {
	if (RAND_bytes(data, int(size)) != 1) {
		std::memset(data, 0, size);
	}
}

// Keystream of AES-256 in the counter mode, the counter is the offset in the file.
void XorStream(
		const Key &key,
		const unsigned char *nonce,
		sqlite3_int64 offset,
		unsigned char *data,
		size_t size) {
	if (!size) {
		return;
	}
	const auto skip = size_t(offset % 16);
	const auto first = uint64_t(offset / 16);
	const auto blocks = (skip + size + 15) / 16;
	auto counters = std::vector<unsigned char>(blocks * 16);
	for (auto i = size_t(0); i != blocks; ++i) {
		auto block = counters.data() + i * 16;
		std::memcpy(block, nonce, 16);
		auto carry = first + i;
		for (auto j = 15; j >= 0 && carry; --j) {
			const auto sum = uint64_t(block[j]) + (carry & 0xFF);
			block[j] = uint8_t(sum & 0xFF);
			carry = (carry >> 8) + (sum >> 8);
		}
	}
	auto stream = std::vector<unsigned char>(blocks * 16 + 16);
	const auto context = EVP_CIPHER_CTX_new();
	auto length = 0;
	if (!context
		|| EVP_EncryptInit_ex(context, EVP_aes_256_ecb(), nullptr, key.data(), nullptr) != 1
		|| EVP_CIPHER_CTX_set_padding(context, 0) != 1
		|| EVP_EncryptUpdate(context, stream.data(), &length, counters.data(), int(counters.size())) != 1) {
		EVP_CIPHER_CTX_free(context);
		std::memset(data, 0, size);
		return;
	}
	EVP_CIPHER_CTX_free(context);
	for (auto i = size_t(0); i != size; ++i) {
		data[i] ^= stream[skip + i];
	}
}

[[nodiscard]] bool EncryptPage(
		const Key &key,
		const unsigned char *salt,
		uint32_t pageNo,
		const unsigned char *plain,
		unsigned char *out) {
	unsigned char *nonce = out + kContentSize;
	unsigned char *tag = nonce + kNonceSize;
	RandomBytes(nonce, kNonceSize);
	std::memset(tag + kTagSize, 0, kReserveBytes - kNonceSize - kTagSize);
	unsigned char aad[20];
	std::memcpy(aad, &pageNo, 4);
	std::memcpy(aad + 4, salt, 16);
	const auto context = EVP_CIPHER_CTX_new();
	auto length = 0;
	auto ok = context
		&& EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
		&& EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, kNonceSize, nullptr) == 1
		&& EVP_EncryptInit_ex(context, nullptr, nullptr, key.data(), nonce) == 1
		&& EVP_EncryptUpdate(context, nullptr, &length, aad, sizeof(aad)) == 1
		&& EVP_EncryptUpdate(context, out, &length, plain, kContentSize) == 1
		&& EVP_EncryptFinal_ex(context, out + length, &length) == 1
		&& EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, kTagSize, tag) == 1;
	EVP_CIPHER_CTX_free(context);
	return ok;
}

[[nodiscard]] bool DecryptPage(
		const Key &key,
		const unsigned char *salt,
		uint32_t pageNo,
		const unsigned char *in,
		unsigned char *plain) {
	const unsigned char *nonce = in + kContentSize;
	unsigned char tag[kTagSize];
	std::memcpy(tag, nonce + kNonceSize, kTagSize);
	unsigned char aad[20];
	std::memcpy(aad, &pageNo, 4);
	std::memcpy(aad + 4, salt, 16);
	const auto context = EVP_CIPHER_CTX_new();
	auto length = 0;
	auto ok = context
		&& EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
		&& EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, kNonceSize, nullptr) == 1
		&& EVP_DecryptInit_ex(context, nullptr, nullptr, key.data(), nonce) == 1
		&& EVP_DecryptUpdate(context, nullptr, &length, aad, sizeof(aad)) == 1
		&& EVP_DecryptUpdate(context, plain, &length, in, kContentSize) == 1
		&& EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, kTagSize, tag) == 1
		&& EVP_DecryptFinal_ex(context, plain + length, &length) == 1;
	EVP_CIPHER_CTX_free(context);
	std::memset(plain + kContentSize, 0, kReserveBytes);
	return ok;
}

[[nodiscard]] bool AllZero(const unsigned char *data, size_t size) {
	for (auto i = size_t(0); i != size; ++i) {
		if (data[i]) {
			return false;
		}
	}
	return true;
}

// The MAC covers the magic, the nonce and the hash of everything after the header.
[[nodiscard]] bool JournalMac(
		File &file,
		const unsigned char *header,
		unsigned char *mac) {
	auto size = sqlite3_int64();
	if (file.real->pMethods->xFileSize(file.real, &size) != SQLITE_OK) {
		return false;
	}
	const auto context = EVP_MD_CTX_new();
	if (!context || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
		EVP_MD_CTX_free(context);
		return false;
	}
	auto chunk = std::vector<unsigned char>(kJournalReadChunk);
	auto ok = true;
	for (auto offset = sqlite3_int64(kJournalHeaderSize); ok && offset < size;) {
		const auto amount = int(std::min<sqlite3_int64>(kJournalReadChunk, size - offset));
		ok = (file.real->pMethods->xRead(file.real, chunk.data(), amount, offset) == SQLITE_OK)
			&& (EVP_DigestUpdate(context, chunk.data(), amount) == 1);
		offset += amount;
	}
	unsigned char digest[32];
	auto digestSize = 0U;
	ok = ok && (EVP_DigestFinal_ex(context, digest, &digestSize) == 1);
	EVP_MD_CTX_free(context);
	if (!ok) {
		return false;
	}
	unsigned char data[8 + 16 + 32];
	std::memcpy(data, header, 8 + 16);
	std::memcpy(data + 8 + 16, digest, 32);
	auto macSize = 0U;
	return HMAC(
		EVP_sha256(),
		file.macKey.data(),
		int(file.macKey.size()),
		data,
		sizeof(data),
		mac,
		&macSize) != nullptr && macSize == kJournalMacSize;
}

// A journal left by a crash is used only when its MAC is right.
[[nodiscard]] bool VerifyJournal(File &file) {
	auto size = sqlite3_int64();
	if (file.real->pMethods->xFileSize(file.real, &size) != SQLITE_OK) {
		return false;
	} else if (!size) {
		return true;
	} else if (size < kJournalHeaderSize) {
		return false;
	}
	unsigned char header[kJournalHeaderSize];
	if (file.real->pMethods->xRead(file.real, header, sizeof(header), 0) != SQLITE_OK
		|| std::memcmp(header, kJournalMagic, 8) != 0) {
		return false;
	}
	unsigned char mac[kJournalMacSize];
	if (!JournalMac(file, header, mac)
		|| CRYPTO_memcmp(mac, header + kJournalMacOffset, kJournalMacSize) != 0) {
		return false;
	}
	std::memcpy(file.salt.data(), header + kJournalNonceOffset, 16);
	file.headerReady = true;
	return true;
}

[[nodiscard]] int UpdateJournalMac(File &file) {
	unsigned char header[kJournalHeaderSize];
	auto rc = file.real->pMethods->xRead(file.real, header, sizeof(header), 0);
	if (rc != SQLITE_OK) {
		return rc;
	}
	unsigned char mac[kJournalMacSize];
	if (!JournalMac(file, header, mac)) {
		return SQLITE_IOERR_FSYNC;
	}
	return file.real->pMethods->xWrite(
		file.real,
		mac,
		kJournalMacSize,
		kJournalMacOffset);
}

void CheckJournal(File &file) {
	if (file.verify) {
		file.verify = false;
		file.rejected = !VerifyJournal(file);
	}
}

// The header of the main file: magic, version, salt and the check of the key.
void MakeMainHeader(File &file, unsigned char *header) {
	std::memset(header, 0, kHeaderSize);
	std::memcpy(header, kMainMagic, 8);
	const auto version = uint32_t(1);
	std::memcpy(header + 8, &version, 4);
	RandomBytes(file.salt.data(), 16);
	std::memcpy(header + 16, file.salt.data(), 16);
	const auto check = Derive("check-v1", file.salt.data(), 16);
	std::memcpy(header + 32, check.data(), 16);
	file.key = Derive("page-v1", file.salt.data(), 16);
}

[[nodiscard]] int ReadHeader(File &file) {
	unsigned char header[64];
	auto rc = file.real->pMethods->xRead(file.real, header, sizeof(header), 0);
	if (rc != SQLITE_OK) {
		return (rc == SQLITE_IOERR_SHORT_READ) ? SQLITE_NOTADB : rc;
	}
	if (std::memcmp(header, kMainMagic, 8) != 0) {
		return SQLITE_NOTADB;
	}
	std::memcpy(file.salt.data(), header + 16, 16);
	const auto check = Derive("check-v1", file.salt.data(), 16);
	if (std::memcmp(header + 32, check.data(), 16) != 0) {
		return SQLITE_NOTADB;
	}
	file.key = Derive("page-v1", file.salt.data(), 16);
	file.headerReady = true;
	return SQLITE_OK;
}

[[nodiscard]] int EnsureMainHeader(File &file) {
	if (file.headerReady) {
		return SQLITE_OK;
	}
	unsigned char header[kHeaderSize];
	MakeMainHeader(file, header);
	const auto rc = file.real->pMethods->xWrite(file.real, header, kHeaderSize, 0);
	if (rc == SQLITE_OK) {
		file.headerReady = true;
	}
	return rc;
}

[[nodiscard]] int ReadPage(File &file, uint32_t pageNo, unsigned char *plain) {
	unsigned char physical[kPageSize];
	const auto offset = sqlite3_int64(kHeaderSize) + sqlite3_int64(pageNo) * kPageSize;
	const auto rc = file.real->pMethods->xRead(file.real, physical, kPageSize, offset);
	if (rc != SQLITE_OK) {
		return rc;
	}
	if (AllZero(physical, kPageSize)) {
		std::memset(plain, 0, kPageSize);
		return SQLITE_OK;
	}
	return DecryptPage(file.key, file.salt.data(), pageNo, physical, plain)
		? SQLITE_OK
		: SQLITE_CORRUPT;
}

[[nodiscard]] int WritePage(File &file, uint32_t pageNo, const unsigned char *plain) {
	if (const auto rc = EnsureMainHeader(file); rc != SQLITE_OK) {
		return rc;
	}
	unsigned char physical[kPageSize];
	if (!EncryptPage(file.key, file.salt.data(), pageNo, plain, physical)) {
		return SQLITE_IOERR_WRITE;
	}
	const auto offset = sqlite3_int64(kHeaderSize) + sqlite3_int64(pageNo) * kPageSize;
	return file.real->pMethods->xWrite(file.real, physical, kPageSize, offset);
}

int Close(sqlite3_file *file) {
	auto self = Self(file);
	auto rc = SQLITE_OK;
	if (self->real && self->real->pMethods) {
		rc = self->real->pMethods->xClose(self->real);
	}
	std::memset(self->key.data(), 0, self->key.size());
	std::memset(self->macKey.data(), 0, self->macKey.size());
	return rc;
}

int Read(sqlite3_file *file, void *buffer, int amount, sqlite3_int64 offset) {
	auto self = Self(file);
	if (self->kind == Kind::Plain) {
		return self->real->pMethods->xRead(self->real, buffer, amount, offset);
	}
	if (self->kind == Kind::Main) {
		auto out = static_cast<unsigned char*>(buffer);
		auto remaining = amount;
		auto position = offset;
		while (remaining > 0) {
			const auto pageNo = uint32_t(position / kPageSize);
			const auto inside = int(position % kPageSize);
			const auto count = std::min(remaining, kPageSize - inside);
			unsigned char plain[kPageSize];
			const auto rc = ReadPage(*self, pageNo, plain);
			if (rc == SQLITE_IOERR_SHORT_READ) {
				std::memset(out, 0, remaining);
				return SQLITE_IOERR_SHORT_READ;
			} else if (rc != SQLITE_OK) {
				return rc;
			}
			std::memcpy(out, plain + inside, count);
			out += count;
			position += count;
			remaining -= count;
		}
		return SQLITE_OK;
	}
	const auto shift = (self->kind == Kind::Journal) ? kJournalHeaderSize : 0;
	if (self->kind == Kind::Journal) {
		CheckJournal(*self);
		if (self->rejected) {
			std::memset(buffer, 0, amount);
			return SQLITE_IOERR_SHORT_READ;
		}
	}
	auto size = sqlite3_int64();
	if (const auto rc = self->real->pMethods->xFileSize(self->real, &size); rc != SQLITE_OK) {
		return rc;
	}
	if (self->kind == Kind::Journal && size < kJournalHeaderSize) {
		std::memset(buffer, 0, amount);
		return SQLITE_IOERR_SHORT_READ;
	}
	const auto rc = self->real->pMethods->xRead(self->real, buffer, amount, offset + shift);
	if (rc != SQLITE_OK && rc != SQLITE_IOERR_SHORT_READ) {
		return rc;
	}
	const auto available = std::max<sqlite3_int64>(0, size - shift - offset);
	const auto got = size_t(std::min<sqlite3_int64>(amount, available));
	if (self->kind == Kind::Journal && !self->headerReady) {
		unsigned char header[kJournalHeaderSize];
		if (self->real->pMethods->xRead(self->real, header, sizeof(header), 0) != SQLITE_OK
			|| std::memcmp(header, kJournalMagic, 8) != 0) {
			return SQLITE_IOERR_READ;
		}
		std::memcpy(self->salt.data(), header + kJournalNonceOffset, 16);
		self->headerReady = true;
	}
	const auto nonce = self->salt.data();
	XorStream(self->key, nonce, offset, static_cast<unsigned char*>(buffer), got);
	if (got < size_t(amount)) {
		std::memset(static_cast<unsigned char*>(buffer) + got, 0, amount - got);
		return SQLITE_IOERR_SHORT_READ;
	}
	return SQLITE_OK;
}

int Write(sqlite3_file *file, const void *buffer, int amount, sqlite3_int64 offset) {
	auto self = Self(file);
	if (self->kind == Kind::Plain) {
		return self->real->pMethods->xWrite(self->real, buffer, amount, offset);
	}
	if (self->kind == Kind::Main) {
		auto in = static_cast<const unsigned char*>(buffer);
		auto remaining = amount;
		auto position = offset;
		while (remaining > 0) {
			const auto pageNo = uint32_t(position / kPageSize);
			const auto inside = int(position % kPageSize);
			const auto count = std::min(remaining, kPageSize - inside);
			unsigned char plain[kPageSize];
			if (count == kPageSize) {
				std::memcpy(plain, in, kPageSize);
			} else {
				const auto rc = ReadPage(*self, pageNo, plain);
				if (rc == SQLITE_IOERR_SHORT_READ) {
					std::memset(plain, 0, kPageSize);
				} else if (rc != SQLITE_OK) {
					return rc;
				}
				std::memcpy(plain + inside, in, count);
			}
			if (const auto rc = WritePage(*self, pageNo, plain); rc != SQLITE_OK) {
				return rc;
			}
			in += count;
			position += count;
			remaining -= count;
		}
		return SQLITE_OK;
	}
	auto data = std::vector<unsigned char>(
		static_cast<const unsigned char*>(buffer),
		static_cast<const unsigned char*>(buffer) + amount);
	auto shift = sqlite3_int64(0);
	if (self->kind == Kind::Journal) {
		shift = kJournalHeaderSize;
		CheckJournal(*self);
		if (self->rejected) {
			// WHY: a journal that failed the check is never extended.
			if (const auto rc = self->real->pMethods->xTruncate(self->real, 0); rc != SQLITE_OK) {
				return rc;
			}
			self->rejected = false;
			self->headerReady = false;
		}
		self->written = true;
		auto size = sqlite3_int64();
		if (const auto rc = self->real->pMethods->xFileSize(self->real, &size); rc != SQLITE_OK) {
			return rc;
		}
		if (size < kJournalHeaderSize) {
			// A new journal gets a new nonce, the keystream is never reused for a new file.
			unsigned char header[kJournalHeaderSize] = {};
			std::memcpy(header, kJournalMagic, 8);
			RandomBytes(header + kJournalNonceOffset, 16);
			if (const auto rc = self->real->pMethods->xWrite(self->real, header, sizeof(header), 0); rc != SQLITE_OK) {
				return rc;
			}
			std::memcpy(self->salt.data(), header + kJournalNonceOffset, 16);
			self->headerReady = true;
		} else if (!self->headerReady) {
			unsigned char header[kJournalHeaderSize];
			if (const auto rc = self->real->pMethods->xRead(self->real, header, sizeof(header), 0); rc != SQLITE_OK) {
				return rc;
			}
			if (std::memcmp(header, kJournalMagic, 8) != 0) {
				return SQLITE_IOERR_WRITE;
			}
			std::memcpy(self->salt.data(), header + kJournalNonceOffset, 16);
			self->headerReady = true;
		}
	}
	const auto nonce = self->salt.data();
	XorStream(self->key, nonce, offset, data.data(), data.size());
	return self->real->pMethods->xWrite(self->real, data.data(), amount, offset + shift);
}

int Truncate(sqlite3_file *file, sqlite3_int64 size) {
	auto self = Self(file);
	switch (self->kind) {
	case Kind::Main:
		return self->real->pMethods->xTruncate(self->real, size + kHeaderSize);
	case Kind::Journal:
		if (!size) {
			self->headerReady = false;
			self->rejected = false;
			self->verify = false;
		}
		return self->real->pMethods->xTruncate(self->real, size ? size + kJournalHeaderSize : 0);
	default:
		return self->real->pMethods->xTruncate(self->real, size);
	}
}

int Sync(sqlite3_file *file, int flags) {
	auto self = Self(file);
	if (self->kind == Kind::Journal && self->written && self->headerReady) {
		if (const auto rc = UpdateJournalMac(*self); rc != SQLITE_OK) {
			return rc;
		}
	}
	return self->real->pMethods->xSync(self->real, flags);
}

int FileSize(sqlite3_file *file, sqlite3_int64 *size) {
	auto self = Self(file);
	if (self->kind == Kind::Journal) {
		CheckJournal(*self);
		if (self->rejected) {
			*size = 0;
			return SQLITE_OK;
		}
	}
	const auto rc = self->real->pMethods->xFileSize(self->real, size);
	if (rc != SQLITE_OK) {
		return rc;
	}
	if (self->kind == Kind::Main) {
		*size = (*size > kHeaderSize) ? (*size - kHeaderSize) : 0;
	} else if (self->kind == Kind::Journal) {
		*size = (*size > kJournalHeaderSize) ? (*size - kJournalHeaderSize) : 0;
	}
	return SQLITE_OK;
}

int Lock(sqlite3_file *file, int level) {
	auto self = Self(file);
	return self->real->pMethods->xLock(self->real, level);
}

int Unlock(sqlite3_file *file, int level) {
	auto self = Self(file);
	return self->real->pMethods->xUnlock(self->real, level);
}

int CheckReservedLock(sqlite3_file *file, int *result) {
	auto self = Self(file);
	return self->real->pMethods->xCheckReservedLock(self->real, result);
}

int FileControl(sqlite3_file *file, int op, void *argument) {
	auto self = Self(file);
	if (op == SQLITE_FCNTL_SIZE_HINT && self->kind == Kind::Main) {
		auto hint = *static_cast<sqlite3_int64*>(argument) + kHeaderSize;
		return self->real->pMethods->xFileControl(self->real, op, &hint);
	}
	return self->real->pMethods->xFileControl(self->real, op, argument);
}

int SectorSize(sqlite3_file *file) {
	auto self = Self(file);
	return std::max(self->real->pMethods->xSectorSize(self->real), kPageSize);
}

int DeviceCharacteristics(sqlite3_file *file) {
	auto self = Self(file);
	// WHY: without the sequential flag every journal sync is done, and
	// every sync seals the journal with its MAC.
	return self->real->pMethods->xDeviceCharacteristics(self->real)
		& ~(SQLITE_IOCAP_ATOMIC | SQLITE_IOCAP_SEQUENTIAL);
}

const sqlite3_io_methods Methods = {
	1,
	Close,
	Read,
	Write,
	Truncate,
	Sync,
	FileSize,
	Lock,
	Unlock,
	CheckReservedLock,
	FileControl,
	SectorSize,
	DeviceCharacteristics,
	nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};

[[nodiscard]] bool UnderPrefix(const char *name) {
	return name && !Prefix.empty() && std::strncmp(name, Prefix.c_str(), Prefix.size()) == 0;
}

int Open(sqlite3_vfs *, const char *name, sqlite3_file *file, int flags, int *outFlags) {
	auto self = new (file) File();
	self->real = reinterpret_cast<sqlite3_file*>(
		reinterpret_cast<char*>(file) + ((sizeof(File) + 7) & ~size_t(7)));
	self->base.pMethods = nullptr;
	auto kind = Kind::Plain;
	if ((flags & SQLITE_OPEN_MAIN_DB) && UnderPrefix(name)) {
		kind = Kind::Main;
	} else if ((flags & SQLITE_OPEN_MAIN_JOURNAL) && UnderPrefix(name)) {
		kind = Kind::Journal;
	} else if (flags & (SQLITE_OPEN_TEMP_DB | SQLITE_OPEN_TEMP_JOURNAL
		| SQLITE_OPEN_TRANSIENT_DB | SQLITE_OPEN_SUBJOURNAL)) {
		kind = Kind::Temp;
	}
	const auto rc = Base->xOpen(Base, name, self->real, flags, outFlags);
	if (rc != SQLITE_OK) {
		return rc;
	}
	self->kind = kind;
	if (kind == Kind::Main) {
		auto size = sqlite3_int64();
		auto status = self->real->pMethods->xFileSize(self->real, &size);
		if (status == SQLITE_OK) {
			if (size == 0) {
				self->headerReady = false;
				RandomBytes(self->salt.data(), 16);
				self->key = Derive("page-v1", self->salt.data(), 16);
			} else {
				status = ReadHeader(*self);
			}
		}
		if (status != SQLITE_OK) {
			self->real->pMethods->xClose(self->real);
			return status;
		}
	} else if (kind == Kind::Journal) {
		self->key = Derive("journal-v1", nullptr, 0);
		self->macKey = Derive("journal-mac-v1", nullptr, 0);
		auto size = sqlite3_int64();
		self->verify = (self->real->pMethods->xFileSize(self->real, &size) == SQLITE_OK)
			&& (size > 0);
	} else if (kind == Kind::Temp) {
		RandomBytes(self->key.data(), self->key.size());
		RandomBytes(self->salt.data(), 16);
	}
	self->base.pMethods = &Methods;
	return SQLITE_OK;
}

int Delete(sqlite3_vfs *, const char *name, int syncDirectory) {
	return Base->xDelete(Base, name, syncDirectory);
}

int Access(sqlite3_vfs *, const char *name, int flags, int *result) {
	return Base->xAccess(Base, name, flags, result);
}

int FullPathname(sqlite3_vfs *, const char *name, int size, char *out) {
	return Base->xFullPathname(Base, name, size, out);
}

void *DlOpen(sqlite3_vfs *, const char *name) {
	return Base->xDlOpen(Base, name);
}

void DlError(sqlite3_vfs *, int size, char *message) {
	Base->xDlError(Base, size, message);
}

void (*DlSym(sqlite3_vfs *, void *handle, const char *symbol))(void) {
	return Base->xDlSym(Base, handle, symbol);
}

void DlClose(sqlite3_vfs *, void *handle) {
	Base->xDlClose(Base, handle);
}

int Randomness(sqlite3_vfs *, int size, char *out) {
	return Base->xRandomness(Base, size, out);
}

int Sleep(sqlite3_vfs *, int microseconds) {
	return Base->xSleep(Base, microseconds);
}

int CurrentTime(sqlite3_vfs *, double *time) {
	return Base->xCurrentTime(Base, time);
}

int GetLastError(sqlite3_vfs *, int size, char *message) {
	return Base->xGetLastError(Base, size, message);
}

int CurrentTimeInt64(sqlite3_vfs *, sqlite3_int64 *time) {
	return Base->xCurrentTimeInt64 ? Base->xCurrentTimeInt64(Base, time) : SQLITE_ERROR;
}

} // namespace

bool Register(const unsigned char *key, size_t keySize, const char *pathPrefix) {
	if (keySize != MasterKey.size() || !pathPrefix) {
		return false;
	}
	const auto lock = std::lock_guard(StateMutex);
	if (!Registered) {
		Base = sqlite3_vfs_find(nullptr);
		if (!Base) {
			return false;
		}
		Vfs = *Base;
		Vfs.iVersion = 2;
		Vfs.szOsFile = int(((sizeof(File) + 7) & ~size_t(7)) + Base->szOsFile);
		Vfs.zName = "freshenc";
		Vfs.pNext = nullptr;
		Vfs.pAppData = nullptr;
		Vfs.xOpen = Open;
		Vfs.xDelete = Delete;
		Vfs.xAccess = Access;
		Vfs.xFullPathname = FullPathname;
		Vfs.xDlOpen = DlOpen;
		Vfs.xDlError = DlError;
		Vfs.xDlSym = DlSym;
		Vfs.xDlClose = DlClose;
		Vfs.xRandomness = Randomness;
		Vfs.xSleep = Sleep;
		Vfs.xCurrentTime = CurrentTime;
		Vfs.xGetLastError = GetLastError;
		Vfs.xCurrentTimeInt64 = CurrentTimeInt64;
		Vfs.xSetSystemCall = nullptr;
		Vfs.xGetSystemCall = nullptr;
		Vfs.xNextSystemCall = nullptr;
		if (sqlite3_vfs_register(&Vfs, 1) != SQLITE_OK) {
			return false;
		}
		Registered = true;
	}
	std::memcpy(MasterKey.data(), key, MasterKey.size());
	auto full = std::vector<char>(4096);
	if (Base->xFullPathname(Base, pathPrefix, int(full.size()), full.data()) != SQLITE_OK) {
		return false;
	}
	Prefix = full.data();
	return true;
}

} // namespace FreshEncryptedVfs
