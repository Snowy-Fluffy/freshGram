// This file is part of freshGram.
#pragma once

#include <QtCore/QString>

#include <string>

namespace FreshStorage {

void MigrateLegacyNames();

// WHY: when the database files could not be moved they stay as they were,
// the database is not opened and the move is retried at the next start.
[[nodiscard]] bool DatabaseMigrated();

// Paths stored before the move still point into the old folders.
[[nodiscard]] QString CurrentPath(const QString &path);
[[nodiscard]] std::string CurrentPath(const std::string &path);

} // namespace FreshStorage
