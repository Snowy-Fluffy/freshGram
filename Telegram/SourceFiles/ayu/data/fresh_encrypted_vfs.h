// This is the source code of freshGram.
#pragma once

#include <cstddef>

namespace FreshEncryptedVfs {

constexpr auto kPageSize = 4096;
// The bytes at the end of every page of the database file: nonce and tag.
constexpr auto kReserveBytes = 32;

// Wraps the default VFS, the files under the prefix are encrypted.
// The main database file is stored page by page with AES-256-GCM, so a changed
// page is detected. Its journal and the temporary files are encrypted with
// AES-256-CTR. The database must use 4096 byte pages and journal_mode
// DELETE or TRUNCATE.
[[nodiscard]] bool Register(
	const unsigned char *key,
	size_t keySize,
	const char *pathPrefix);

} // namespace FreshEncryptedVfs
