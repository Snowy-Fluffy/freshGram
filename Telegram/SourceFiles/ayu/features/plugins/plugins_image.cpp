// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_image.h"

#ifdef FRESHGRAM_PLUGINS

#include <QtCore/QBuffer>
#include <QtGui/QImage>
#include <QtGui/QImageReader>
#include <QtGui/QImageWriter>
#include <QtGui/QPainter>

#include <algorithm>

namespace FreshPlugins {
namespace {

// The picture comes from a plugin, so it is checked before it is decoded.
constexpr auto kMaxPixels = qint64(24) * 1000 * 1000;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
constexpr auto kAllocationLimitMb = 256;
#endif // Qt >= 6.0
constexpr auto kMaxSide = qint64(8192);

namespace error = fgpl::api::error;

[[nodiscard]] QSize Fitted(QSize source, qint64 width, qint64 height) {
	if (!width) {
		width = std::max<qint64>(source.width() * height / source.height(), 1);
	} else if (!height) {
		height = std::max<qint64>(source.height() * width / source.width(), 1);
	}
	return QSize(int(width), int(height));
}

[[nodiscard]] QRect CoverPart(QSize source, QSize target) {
	const auto sw = qint64(source.width());
	const auto sh = qint64(source.height());
	const auto tw = qint64(target.width());
	const auto th = qint64(target.height());
	const auto size = (sw * th > sh * tw)
		? QSize(int(std::clamp<qint64>(sh * tw / th, 1, sw)), int(sh))
		: QSize(int(sw), int(std::clamp<qint64>(sw * th / tw, 1, sh)));
	return QRect(
		QPoint(
			(source.width() - size.width()) / 2,
			(source.height() - size.height()) / 2),
		size);
}

} // namespace

fgpl::Result<fgpl::ImageResult, std::string> ProcessImage(
		const fgpl::ImageRequest &request) {
	using Failure = std::string;
	auto bytes = QByteArray::fromRawData(
		reinterpret_cast<const char*>(request.data.data()),
		int(request.data.size()));
	auto input = QBuffer(&bytes);
	if (!input.open(QIODevice::ReadOnly)) {
		return Failure(error::kFailed);
	}
	auto reader = QImageReader(&input);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	reader.setAllocationLimit(kAllocationLimitMb);
#endif // Qt >= 6.0
	const auto format = reader.format();
	if (format != "png" && format != "jpeg" && format != "webp") {
		return Failure(error::kInvalid);
	} else if (reader.supportsAnimation() && reader.imageCount() != 1) {
		return Failure(error::kInvalid);
	}
	const auto size = reader.size();
	if (size.isEmpty()) {
		return Failure(error::kInvalid);
	} else if (qint64(size.width()) * size.height() > kMaxPixels) {
		return Failure(error::kLimit);
	}
	auto result = fgpl::ImageResult{
		.format = format.toStdString(),
		.width = size.width(),
		.height = size.height(),
	};
	if (request.op == "info") {
		return result;
	}
	auto image = reader.read();
	if (image.isNull()) {
		return Failure(error::kFailed);
	}
	if (request.op == "resize") {
		const auto target = Fitted(image.size(), request.width, request.height);
		if (target.width() > kMaxSide || target.height() > kMaxSide) {
			return Failure(error::kLimit);
		}
		if (request.fit == "cover") {
			image = image.copy(CoverPart(image.size(), target));
		}
		const auto mode = (request.fit == "exact" || request.fit == "cover")
			? Qt::IgnoreAspectRatio
			: Qt::KeepAspectRatio;
		image = image.scaled(target, mode, Qt::SmoothTransformation);
	} else if (request.op == "crop") {
		const auto part = QRect(
			QPoint(int(request.x), int(request.y)),
			QSize(int(request.width), int(request.height))
		).intersected(image.rect());
		if (part.isEmpty()) {
			return Failure(error::kInvalid);
		}
		image = image.copy(part);
	} else if (request.op != "convert") {
		return Failure(error::kInvalid);
	}
	const auto outFormat = request.format.empty() ? result.format : request.format;
	if (outFormat == "jpeg" && image.hasAlphaChannel()) {
		auto flat = QImage(image.size(), QImage::Format_RGB32);
		flat.fill(Qt::white);
		auto p = QPainter(&flat);
		p.drawImage(0, 0, image);
		p.end();
		image = std::move(flat);
	}
	auto encoded = QByteArray();
	auto output = QBuffer(&encoded);
	if (!output.open(QIODevice::WriteOnly)) {
		return Failure(error::kFailed);
	}
	auto writer = QImageWriter(&output, QByteArray::fromStdString(outFormat));
	writer.setQuality(int(request.quality));
	if (!writer.canWrite()) {
		return Failure(error::kUnavailable);
	} else if (!writer.write(image)) {
		return Failure(error::kFailed);
	}
	result.format = outFormat;
	result.width = image.width();
	result.height = image.height();
	result.data.assign(
		reinterpret_cast<const uint8_t*>(encoded.constData()),
		reinterpret_cast<const uint8_t*>(encoded.constData()) + encoded.size());
	return result;
}

} // namespace FreshPlugins

#endif // FRESHGRAM_PLUGINS
