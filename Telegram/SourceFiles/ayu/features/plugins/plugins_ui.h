// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"
#include "ui/rp_widget.h"

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

namespace Ui::Menu {
struct MenuCallback;
} // namespace Ui::Menu

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

void ShowPluginWindow(
	const std::string &id,
	const std::string &account,
	const std::string &window,
	const std::string &mode,
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
	bool official = false;
	QByteArray icon;
};

[[nodiscard]] PluginHeader HeaderFor(
	const fgpl::Manifest &manifest,
	const std::string &publisher,
	QByteArray icon);
void AddPluginHeader(
	not_null<Ui::VerticalLayout*> container,
	const PluginHeader &data);

[[nodiscard]] QString PermissionTitle(
	const std::string &id,
	const std::string &scope);
[[nodiscard]] QString PermissionLabel(const std::string &id);
[[nodiscard]] QString AcceptsFrom(const std::vector<std::string> &senders);
[[nodiscard]] QString LimitsText(const fgpl::Manifest &manifest);
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

class ComposerButtons final : public Ui::RpWidget {
public:
	ComposerButtons(
		QWidget *parent,
		not_null<Window::SessionController*> controller);
	~ComposerButtons();

	bool refresh(PeerData *peer, MsgId topicRootId);
	[[nodiscard]] bool empty() const;
	void setReply(Fn<MsgId()> current, Fn<void()> taken);

private:
	struct State;
	const std::unique_ptr<State> _state;

};
void AddTrayItems(Fn<void(const QString &text, Fn<void()> handler)> add);
[[nodiscard]] QString TrayItemsKey();
void ShowNetworkPanel(not_null<Window::SessionController*> controller);
void ShowNetworkLog(not_null<Window::SessionController*> controller);

void AddMessageMenuItems(
	not_null<Ui::PopupMenu*> menu,
	not_null<HistoryItem*> item);

void AddPeerMenuItems(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	MsgId topicRootId,
	bool profile,
	const Ui::Menu::MenuCallback &add);

void AddMainMenuItems(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

struct PluginCommand {
	QString name;
	QString description;
	QString plugin;
	bool ambiguous = false;
};

[[nodiscard]] std::vector<PluginCommand> CommandsFor(
	not_null<Main::Session*> session,
	not_null<PeerData*> peer);

enum class CommandResult {
	None,
	Sent,
	Refused,
};

[[nodiscard]] CommandResult RunCommand(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	MsgId topicRootId,
	MsgId replyToId,
	const QString &text);

void ShowPluginSettings(
	not_null<Window::SessionController*> controller,
	const QString &pluginId);

void ShowPluginDetails(
	not_null<Window::SessionController*> controller,
	const QString &pluginId);

} // namespace FreshPlugins
