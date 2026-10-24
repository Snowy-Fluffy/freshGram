// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"

class HistoryItem;
class PeerData;

namespace Main {
class Session;
} // namespace Main

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

namespace FreshPlugins {

void StoreSettingsPage(
	const std::string &id,
	const std::string &account,
	const fgpl::cbor::Value &page);

void ForgetSettingsPages(const std::string &id);

void ShowPluginDialog(
	const std::string &id,
	const std::string &account,
	const fgpl::cbor::Value *page);

[[nodiscard]] QString PermissionLabel(const std::string &id);
[[nodiscard]] QString AcceptsFrom(const std::vector<std::string> &senders);
void AddPermissionDanger(
	not_null<Ui::VerticalLayout*> layout,
	const std::string &id,
	const std::string &scope,
	const QString &reason);
[[nodiscard]] bool HasComposerItems(not_null<Main::Session*> session);
void ShowComposerMenu(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	not_null<QWidget*> button);
void AddTrayItems(Fn<void(const QString &text, Fn<void()> handler)> add);
void ShowNetworkPanel(not_null<Window::SessionController*> controller);

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

} // namespace FreshPlugins
