// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/ayu_logo.h"

#include "ayu/ayu_settings.h"
#include "styles/style_ayu_styles.h"
#include "ui/rect.h"

#include <array>

#include <QBuffer>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QImageReader>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSvgRenderer>

static QString LAST_LOADED_NAME;
static QImage LAST_LOADED;
static QImage LAST_LOADED_PAD;

namespace AyuAssets {
namespace {

constexpr auto kCustomIconSize = 512;
constexpr auto kIcoSizes = std::array{ 16, 24, 32, 48, 64, 128, 256 };

[[nodiscard]] QString CustomPrefix() {
	return u"custom:"_q;
}

[[nodiscard]] QString CustomIconsDir() {
	return cWorkingDir() + u"tdata/custom_icons/"_q;
}

// The id is a file name, so it must never reach outside the folder!!!
[[nodiscard]] bool IsValidCustomId(const QString &id) {
	static const auto re = QRegularExpression(u"^[a-z0-9_-]{1,64}$"_q);
	return re.match(id).hasMatch();
}

[[nodiscard]] QString CustomId(const QString &name) {
	return name.mid(CustomPrefix().size());
}

[[nodiscard]] QImage SquareIcon(const QImage &image, int size) {
	const auto side = std::min(image.width(), image.height());
	const auto cropped = image.copy(
		(image.width() - side) / 2,
		(image.height() - side) / 2,
		side,
		side);
	auto result = cropped.convertToFormat(
		QImage::Format_ARGB32_Premultiplied
	).scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	result.setDevicePixelRatio(1.);
	return result;
}

[[nodiscard]] QImage ReadSvg(const QString &path, const QByteArray &content) {
	auto svg = QSvgRenderer();
	const auto loaded = content.isEmpty()
		? svg.load(path)
		: svg.load(content);
	if (!loaded || !svg.isValid()) {
		return {};
	}
	auto size = svg.defaultSize();
	if (size.isEmpty()) {
		size = QSize(kCustomIconSize, kCustomIconSize);
	}
	size.scale(kCustomIconSize, kCustomIconSize, Qt::KeepAspectRatio);
	auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	{
		auto p = QPainter(&result);
		svg.render(&p, QRect(QPoint(), size));
	}
	return result;
}

[[nodiscard]] QImage ReadRaster(const QString &path, const QByteArray &content) {
	auto buffer = QBuffer();
	auto reader = QImageReader();
	if (content.isEmpty()) {
		reader.setFileName(path);
	} else {
		buffer.setData(content);
		buffer.open(QIODevice::ReadOnly);
		reader.setDevice(&buffer);
	}
	reader.setAutoTransform(true);
	return reader.read();
}

[[nodiscard]] bool LooksLikeSvg(
		const QString &path,
		const QByteArray &content) {
	if (path.endsWith(u".svg"_q, Qt::CaseInsensitive)) {
		return true;
	}
	return content.left(1024).contains("<svg");
}

[[nodiscard]] QImage ReadIconSource(
		const QString &path,
		const QByteArray &content) {
	return LooksLikeSvg(path, content)
		? ReadSvg(path, content)
		: ReadRaster(path, content);
}

bool WriteIco(const QImage &image, const QString &path) {
	if (image.isNull()) {
		return false;
	}
	auto pngs = std::vector<QByteArray>();
	for (const auto size : kIcoSizes) {
		auto bytes = QByteArray();
		auto buffer = QBuffer(&bytes);
		buffer.open(QIODevice::WriteOnly);
		if (!SquareIcon(image, size).save(&buffer, "PNG")) {
			return false;
		}
		pngs.push_back(std::move(bytes));
	}

	auto result = QByteArray();
	{
		auto stream = QDataStream(&result, QIODevice::WriteOnly);
		stream.setByteOrder(QDataStream::LittleEndian);
		stream << quint16(0) << quint16(1) << quint16(pngs.size());

		auto offset = quint32(6 + 16 * pngs.size());
		auto index = 0;
		for (const auto size : kIcoSizes) {
			const auto &png = pngs[index++];
			const auto side = quint8((size >= 256) ? 0 : size);
			stream
				<< side
				<< side
				<< quint8(0) // colors in palette
				<< quint8(0) // reserved
				<< quint16(1) // color planes
				<< quint16(32) // bits per pixel
				<< quint32(png.size())
				<< offset;
			offset += quint32(png.size());
		}
		for (const auto &png : pngs) {
			stream.writeRawData(png.constData(), png.size());
		}
	}

	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	file.write(result);
	return file.commit();
}

}

bool isCustomIcon(const QString &name) {
	return name.startsWith(CustomPrefix());
}

QString customIconPath(const QString &name) {
	if (!isCustomIcon(name)) {
		return QString();
	}
	const auto id = CustomId(name);
	return IsValidCustomId(id)
		? (CustomIconsDir() + id + u".png"_q)
		: QString();
}

QStringList customIcons() {
	auto result = QStringList();
	const auto list = QDir(CustomIconsDir()).entryInfoList(
		{ u"*.png"_q },
		QDir::Files,
		QDir::Name);
	for (const auto &info : list) {
		const auto id = info.completeBaseName();
		if (IsValidCustomId(id)) {
			result.push_back(CustomPrefix() + id);
		}
	}
	return result;
}

QString importCustomIcon(const QString &path, const QByteArray &content) {
	if (path.isEmpty() && content.isEmpty()) {
		return QString();
	}
	const auto image = ReadIconSource(path, content);
	if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
		return QString();
	}
	const auto dir = CustomIconsDir();
	if (!QDir().mkpath(dir)) {
		return QString();
	}

	auto bytes = QByteArray();
	{
		auto buffer = QBuffer(&bytes);
		buffer.open(QIODevice::WriteOnly);
		if (!SquareIcon(image, kCustomIconSize).save(&buffer, "PNG")) {
			return QString();
		}
	}

	for (const auto &existing : customIcons()) {
		auto file = QFile(customIconPath(existing));
		if (file.size() == bytes.size()
			&& file.open(QIODevice::ReadOnly)
			&& file.readAll() == bytes) {
			return existing;
		}
	}

	const auto base = QString::number(
		QDateTime::currentMSecsSinceEpoch(),
		36);
	auto id = base;
	for (auto i = 1; QFile::exists(dir + id + u".png"_q); ++i) {
		id = base + QChar('_') + QString::number(i);
	}

	auto file = QSaveFile(dir + id + u".png"_q);
	if (!file.open(QIODevice::WriteOnly)
		|| file.write(bytes) != bytes.size()
		|| !file.commit()) {
		return QString();
	}
	return CustomPrefix() + id;
}

void removeCustomIcon(const QString &name) {
	const auto path = customIconPath(name);
	if (path.isEmpty()) {
		return;
	}
	QFile::remove(path);
	QFile::remove(cWorkingDir()
		+ u"tdata/AyuGram-custom-"_q
		+ CustomId(name)
		+ u".ico"_q);
	if (LAST_LOADED_NAME == name) {
		LAST_LOADED_NAME = QString();
	}
}

QString resolvedIconName(const QString &name) {
	if (!isCustomIcon(name)) {
		return name;
	}
	const auto path = customIconPath(name);
	return (!path.isEmpty() && QFile::exists(path)) ? name : DEFAULT_ICON;
}

QString appIcoPath() {
	const auto &settings = AyuSettings::getInstance();
	auto name = resolvedIconName(settings.appIcon());
	return cWorkingDir()
		+ u"tdata/AyuGram-"_q
		+ name.replace(QChar(':'), QChar('-'))
		+ u".ico"_q;
}

void loadAppIco() {
	const auto &settings = AyuSettings::getInstance();
	const auto name = resolvedIconName(settings.appIcon());
	const auto iconPath = appIcoPath();

	auto f = QFile(iconPath);
	if (f.exists()) {
		f.setPermissions(QFile::WriteOther);
		f.remove();
	}
	f.close();
	if (isCustomIcon(name)) {
		WriteIco(QImage(customIconPath(name)), iconPath);
	} else {
		QFile::copy(qsl(":/gui/art/ayu/%1/app_icon.ico").arg(name), iconPath);
	}
}

QImage CreateImage(const QString &requested, const QSize resultImageSize, const int padding = 0) {
	const auto name = resolvedIconName(requested);
	const auto iconSize = resultImageSize.shrunkBy(QMargins(padding, padding, padding, padding));

	const auto pngPath = isCustomIcon(name)
		? customIconPath(name)
		: qsl(":/gui/art/ayu/%1/app.png").arg(name);
	if (QFile::exists(pngPath)) {
		const auto loaded = QImage(pngPath).scaled(iconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
		auto res = QImage(
			resultImageSize * style::DevicePixelRatio(),
			QImage::Format_ARGB32_Premultiplied);
		res.setDevicePixelRatio(style::DevicePixelRatio());
		res.fill(Qt::transparent);
		{
			auto p = QPainter(&res);
			p.drawImage(QRect(padding, padding, iconSize.width(), iconSize.height()), loaded);
		}
		return res;
	}

	const auto svgPath = qsl(":/gui/art/ayu/%1/app.svg").arg(name);
	if (!QFile::exists(svgPath)) {
		return {};
	}

	auto svg = QSvgRenderer(svgPath);
	auto image = QImage(
		resultImageSize * style::DevicePixelRatio(),
		QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(style::DevicePixelRatio());
	image.fill(Qt::transparent);
	{
		auto p = QPainter(&image);

		QPainterPath path;
		path.addRoundedRect(
			QRect(padding, padding, iconSize.width(), iconSize.height()),
			iconSize.width() / 2.0f,
			iconSize.height() / 2.0f
		);

		p.save();

		p.setRenderHint(QPainter::Antialiasing, true);
		p.setClipPath(path);
		p.setRenderHint(QPainter::Antialiasing, false);

		svg.render(&p, QRect(padding, padding, iconSize.width(), iconSize.height()));

		p.restore();
	}
	return image;
}

void loadIcons() {
	const auto &settings = AyuSettings::getInstance();
	if (LAST_LOADED_NAME != settings.appIcon()) {
		LAST_LOADED_NAME = settings.appIcon();
		LAST_LOADED = CreateImage(settings.appIcon(), Size(256));
		LAST_LOADED_PAD = CreateImage(settings.appIcon(), Size(256), 12);
	}
}

QImage loadPreview(const QString &name) {
	return CreateImage(name, Size(st::iconPickerIconSize), st::iconPickerImagePadding);
}

QImage appLogo(const QString &name, int size) {
	return CreateImage(name, Size(size)).scaled(
		size,
		size,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation);
}

QString currentAppLogoName() {
	return LAST_LOADED_NAME;
}

QImage currentAppLogo() {
	loadIcons();
	return LAST_LOADED;
}

QImage currentAppLogoPad() {
	loadIcons();
	return LAST_LOADED_PAD;
}

}
