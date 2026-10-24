// This is the source code of freshGram.
#pragma once

#include <string>

struct sqlite3;

namespace FreshMigrationV4 {

struct Result {
	bool ok = false;
	bool migrated = false;
	long long deleted = 0;
	long long edited = 0;
	double milliseconds = 0.;
	std::string error;
};

// Copies the wide message tables of the schema into the compact ones of the
// main database. The tables of the main schema are dropped after the copy,
// the other ones (an attached old database) are left as they are.
// The compact tables must exist already, the caller owns the transaction.
[[nodiscard]] Result Run(sqlite3 *db, const char *schema = "main");

} // namespace FreshMigrationV4
