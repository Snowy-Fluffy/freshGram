// This file is part of freshGram.
#pragma once

#include <string>

namespace FreshStorage {

// WHY: kept apart from fresh_storage_migration.h so that code which is
// built without Qt, like the secret chat protocol tests, can include it.
// Paths stored before the move still point into the old folders.
[[nodiscard]] std::string CurrentPath(const std::string &path);

} // namespace FreshStorage
