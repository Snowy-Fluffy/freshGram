// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "base/qthelp_regex.h"
#include "window/window_session_controller.h"

#include <QtCore/QUrl>

namespace AyuUrlHandlers {

using Match = qthelp::RegularExpressionMatch;

bool ResolveUser(
	Window::SessionController *controller,
	const Match &match,
	const QVariant &context);

bool ResolveChat(
	Window::SessionController *controller,
	const Match &match,
	const QVariant &context);

bool HandleAyu(
	Window::SessionController *controller,
	const Match &match,
	const QVariant &context);

bool HandleAyuSettings(
	Window::SessionController *controller,
	const Match &match,
	const QVariant &context);

// tg://ayuplugin?id=<plugin>&alias=<item>: a plugin's settings, scrolled to one item.
bool HandleAyuPlugin(
	Window::SessionController *controller,
	const Match &match,
	const QVariant &context);

// tg://plugin/<id>/<path>?<query>: a link a plugin declared in its manifest.
bool HandlePluginLink(
	Window::SessionController *controller,
	const Match &match,
	const QVariant &context);

bool TryHandleSpotify(const QString &url);

[[nodiscard]] bool IsWebUrl(const QUrl &url);
[[nodiscard]] bool IsUnsafeExternalUrl(const QString &url);

}
