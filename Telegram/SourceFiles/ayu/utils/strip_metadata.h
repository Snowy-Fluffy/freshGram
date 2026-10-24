// This is the source code of freshGram.
#pragma once

#include <optional>

namespace FreshMetadata {

enum class MediaKind : uchar {
	None,
	Image,
	Video,
};

[[nodiscard]] MediaKind KindOf(const QString &path, const QString &mime);

[[nodiscard]] std::optional<QByteArray> StripImage(const QByteArray &bytes);

[[nodiscard]] QImage StripImageText(const QImage &image);

[[nodiscard]] bool StripVideo(const QString &source, const QString &target);

[[nodiscard]] QString NewTempVideoPath(const QString &extension);

} // namespace FreshMetadata
