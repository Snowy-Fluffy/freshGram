// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#define ICON(name, value) const auto name##_ICON = QStringLiteral(value)

namespace AyuAssets {

ICON(DEFAULT, "default");
ICON(ALT, "alt");
ICON(DISCORD, "discord");
ICON(SPOTIFY, "spotify");
ICON(EXTERA, "extera");
ICON(NOTHING, "nothing");
ICON(BARD, "bard");
ICON(YAPLUS, "yaplus");
ICON(WIN95, "win95");
ICON(CHIBI, "chibi");
ICON(CHIBI2, "chibi2");
ICON(EXTERA2, "extera2");
ICON(PLANE, "plane");
ICON(WHEELOLD, "wheelold");
ICON(SEGMENT, "segment");

// Icons the user uploads live in tdata/custom_icons/<id>.png
// and are stored in the appIcon setting as "custom:<id>"
[[nodiscard]] bool isCustomIcon(const QString &name);
[[nodiscard]] QString customIconPath(const QString &name);
[[nodiscard]] QStringList customIcons();

// Returns the new icon name, or an empty string if the image is unreadable
[[nodiscard]] QString importCustomIcon(
	const QString &path,
	const QByteArray &content);
void removeCustomIcon(const QString &name);

// A custom icon whose file is gone falls back to the default one
[[nodiscard]] QString resolvedIconName(const QString &name);

void loadAppIco();
QString appIcoPath();

QImage loadPreview(const QString& name);
QImage appLogo(const QString &name, int size);

QString currentAppLogoName();
QImage currentAppLogo();
QImage currentAppLogoPad();

}
