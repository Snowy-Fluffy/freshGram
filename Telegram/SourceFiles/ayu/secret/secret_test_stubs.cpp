// This file is part of freshGram.
//
// Stands in for the Qt based storage migration in the standalone protocol
// tests, which are built without Qt.
#include "ayu/utils/fresh_storage_path.h"

namespace FreshStorage {

std::string CurrentPath(const std::string &path) {
	return path;
}

} // namespace FreshStorage
