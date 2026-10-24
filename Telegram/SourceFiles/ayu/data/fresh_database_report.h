// This is the source code of freshGram.
#pragma once

#include <string>

struct sqlite3;

namespace FreshDatabaseReport {

struct Options {
	// One full copy of the database, the same call the snapshot makes.
	bool measureSerialize = true;
	bool measureCrypto = true;
};

// Aggregates only, no message text, names or identifiers are printed.
[[nodiscard]] std::string Build(sqlite3 *db, const Options &options = {});

// AES-256-GCM over 4 KiB pages, the cost of the page encryption.
[[nodiscard]] std::string MeasureCrypto();

} // namespace FreshDatabaseReport
