// This is the source code of freshGram.
#pragma once

#include <string>

struct sqlite3;

namespace FreshLegacyCopy {

struct Result {
	bool ok = false;
	long long deleted = 0;
	long long edited = 0;
	long long others = 0;
	double milliseconds = 0.;
	std::string error;
};

// Copies the data of an attached old database into the tables of the main one.
// The tables of the main schema must exist, the caller owns the transaction.
[[nodiscard]] Result Run(sqlite3 *db, const char *schema);

} // namespace FreshLegacyCopy
