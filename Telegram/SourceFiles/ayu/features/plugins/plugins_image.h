// This is the source code of freshGram.
#pragma once

#ifdef FRESHGRAM_PLUGINS

#include "fgpl/broker.h"

namespace FreshPlugins {

// Decodes, changes and encodes a picture of a plugin; works on any thread.
[[nodiscard]] fgpl::Result<fgpl::ImageResult, std::string> ProcessImage(
	const fgpl::ImageRequest &request);

} // namespace FreshPlugins

#endif // FRESHGRAM_PLUGINS
