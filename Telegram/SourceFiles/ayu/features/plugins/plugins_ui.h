// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"

class HistoryItem;
class PeerData;

namespace fgpl::cbor {
class Value;
} // namespace fgpl::cbor

namespace Ui {
class PopupMenu;
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace AyuPlugins {

void StoreSettingsPage(
	const std::string &id,
	const std::string &account,
	const fgpl::cbor::Value &page);

void ForgetSettingsPages(const std::string &id);

void AddMessageMenuItems(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryItem*> item);

void AddPeerMenuItems(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	bool profile,
	Fn<void(const QString &title, Fn<void()> handler)> add);

void AddMainMenuItems(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

[[nodiscard]] bool TryRunCommand(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	const QString &text);

void ShowPluginSettings(
	not_null<Window::SessionController*> controller,
	const QString &pluginId);

void ShowPluginDetails(
	not_null<Window::SessionController*> controller,
	const QString &pluginId);

} // namespace AyuPlugins
