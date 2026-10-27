// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"
#include "data/data_msg_id.h"
#include "ui/rp_widget.h"
#include "ui/click_handler.h"

class QPainter;

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

struct PluginBadge {
	std::string plugin;
	std::string id;
	std::string icon;
	QByteArray image;
	QString tooltip;

	friend inline bool operator==(
		const PluginBadge &a,
		const PluginBadge &b) = default;
};

// WHY: no items drop the plugin's badges of that user, and user 0 with no
// items drops all of them.
void StoreBadges(
	const std::string &plugin,
	const std::string &account,
	int64_t user,
	const fgpl::cbor::Value *items);
void ForgetBadges(const std::string &plugin);

[[nodiscard]] std::vector<PluginBadge> BadgesFor(not_null<PeerData*> peer);
[[nodiscard]] rpl::producer<> BadgesChanged();
[[nodiscard]] int BadgesWidth(
	const std::vector<PluginBadge> &badges,
	int side);
void PaintBadges(
	QPainter &p,
	int left,
	int top,
	int side,
	int outerWidth,
	const std::vector<PluginBadge> &badges,
	QColor color);
[[nodiscard]] QRect BadgeRect(int index, int left, int top, int side);
[[nodiscard]] int BadgeIndexAt(
	const std::vector<PluginBadge> &badges,
	int side,
	int x);
void ClickBadge(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	const PluginBadge &badge,
	const char *surface,
	FullMsgId item = FullMsgId());
// The list handles clicks on a row's badges, so it needs the painted rects.
void NoteListBadges(const void *row, std::vector<QRect> rects);
[[nodiscard]] int ListBadgeAt(const void *row, QPoint point);
[[nodiscard]] ClickHandlerPtr BadgeLink(
	not_null<PeerData*> peer,
	const PluginBadge &badge,
	const char *surface,
	FullMsgId item = FullMsgId());

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

void AddSelectionMenuItems(
	not_null<Ui::PopupMenu*> menu,
	not_null<Main::Session*> session,
	const std::vector<FullMsgId> &ids);

void AddSharedMediaMenuItems(
	not_null<Ui::PopupMenu*> menu,
	not_null<Main::Session*> session,
	const std::vector<FullMsgId> &ids);

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
