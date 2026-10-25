// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"

class HistoryItem;
class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace fgpl {
struct GrantDecision;
struct Manifest;
} // namespace fgpl

namespace fgpl::cbor {
class Value;
} // namespace fgpl::cbor

namespace Ui {
class GenericBox;
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

struct PluginHeader {
	QString name;
	QString version;
	QString author;
	QString description;
	QString license;
	QString homepage;
	QString repository;
	QString support;
	QString publisher;
	bool signedPackage = false;
	QByteArray icon;
};

[[nodiscard]] PluginHeader HeaderFor(
	const fgpl::Manifest &manifest,
	const std::string &publisher,
	bool signedPackage,
	QByteArray icon);
void AddPluginHeader(
	not_null<Ui::VerticalLayout*> container,
	const PluginHeader &data);

[[nodiscard]] QString PermissionTitle(
	const std::string &id,
	const std::string &scope);
[[nodiscard]] QString PermissionLabel(const std::string &id);
[[nodiscard]] QString AcceptsFrom(const std::vector<std::string> &senders);
// Returns the choice, or nothing after telling the user what is missing.
[[nodiscard]] Fn<std::optional<fgpl::GrantDecision>()> AddPermissionsEditor(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller,
	const fgpl::Manifest &manifest,
	const std::optional<fgpl::GrantDecision> &current,
	const std::vector<std::string> &added);
[[nodiscard]] bool HasComposerItems(
	not_null<Main::Session*> session,
	not_null<PeerData*> peer);
void ShowComposerMenu(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	not_null<QWidget*> button);
void AddTrayItems(Fn<void(const QString &text, Fn<void()> handler)> add);
[[nodiscard]] QString TrayItemsKey();
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
