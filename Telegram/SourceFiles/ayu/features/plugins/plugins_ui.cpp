// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_ui.h"

#include "lang/lang_keys.h"
#include "ayu/features/plugins/plugins_service.h"
#include "ayu/features/plugins/plugins_transport.h"
#include "ayu/secret/secret_peer.h"
#include "core/application.h"
#include "crl/crl_on_main.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "ui/text/format_values.h"
#include "window/window_controller.h"
#include "ayu/ui/boxes/plugin_install_box.h"
#include "boxes/filters/edit_filter_chats_list.h"
#include "boxes/peer_list_box.h"
#include "ayu/ui/fagram/fagram_nav_drawer.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer_bot_command.h"
#include "data/data_peer_bot_commands.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "settings/detailed_settings_button.h"
#include "settings/settings_common.h"
#include "styles/style_ayu_styles.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_window.h"
#include "ui/boxes/confirm_box.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/toast/toast.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/basic_click_handlers.h"
#include "window/window_peer_menu.h"
#include "data/data_thread.h"
#include "styles/style_chat_helpers.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/menu/menu_action.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/tooltip.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/manager.h"
#include "fgpl/permissions.h"
#endif

#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtSvg/QSvgRenderer>

#include <map>

namespace FreshPlugins {

#ifdef FRESHGRAM_PLUGINS

namespace {

namespace cbor = fgpl::cbor;

constexpr auto kMaxPluginChats = 100;
constexpr auto kSettingsWaitMs = crl::time(5000);

constexpr auto kMaxComposerButtons = 3;
constexpr auto kNavDrawerIconLeft = 28;

struct Entry {
	std::string plugin;
	std::string menuId;
	QString text;
	std::string icon;
};

struct Pages {
	std::map<std::pair<std::string, std::string>, cbor::Value> pages;
	rpl::event_stream<std::pair<std::string, std::string>> changes;
};

[[nodiscard]] Pages &GetPages() {
	static auto result = Pages();
	return result;
}

[[nodiscard]] std::string Language() {
	return Lang::Id().toStdString();
}

[[nodiscard]] cbor::Value Text(const QString &text) {
	return cbor::Value::FromText(text.toStdString());
}

[[nodiscard]] QString Utf8(const std::string &text) {
	return QString::fromStdString(text);
}

[[nodiscard]] std::string AccountOf(not_null<Main::Session*> session) {
	return AccountId(session).toStdString();
}

[[nodiscard]] cbor::Value Number(int64_t value) {
	return cbor::Value::FromInt(value);
}

[[nodiscard]] cbor::Value Flag(bool value) {
	return cbor::Value::FromBool(value);
}

[[nodiscard]] const style::icon *BuiltinIcon(const std::string &name) {
	static const auto icons = std::map<std::string, const style::icon*>{
		{ "add", &st::menuIconAdd },
		{ "archive", &st::menuIconArchive },
		{ "block", &st::menuIconBlock },
		{ "bot", &st::menuIconBot },
		{ "cancel", &st::menuIconCancel },
		{ "chat", &st::menuIconChatBubble },
		{ "chats", &st::menuIconChats },
		{ "clear", &st::menuIconClear },
		{ "collapse", &st::menuIconCollapse },
		{ "commands", &st::menuIconBotCommands },
		{ "copy", &st::menuIconCopy },
		{ "customize", &st::menuIconCustomize },
		{ "delete", &st::menuIconDelete },
		{ "devices", &st::menuIconDevices },
		{ "download", &st::menuIconDownload },
		{ "edit", &st::menuIconEdit },
		{ "emoji", &st::menuIconEmoji },
		{ "expand", &st::menuIconExpand },
		{ "export", &st::menuIconExport },
		{ "file", &st::menuIconFile },
		{ "folder", &st::menuIconShowInFolder },
		{ "font", &st::menuIconFont },
		{ "forward", &st::menuIconForward },
		{ "gif", &st::menuIconGif },
		{ "hide", &st::menuIconUserHide },
		{ "hourglass", &st::menuIconHourglass },
		{ "info", &st::menuIconInfo },
		{ "like", &st::menuIconLike },
		{ "link", &st::menuIconLink },
		{ "lock", &st::menuIconLock },
		{ "mark_read", &st::menuIconMarkRead },
		{ "mute", &st::menuIconMute },
		{ "network", &st::menuIconNetwork },
		{ "night", &st::menuIconNightMode },
		{ "notifications", &st::menuIconNotifications },
		{ "palette", &st::menuIconPalette },
		{ "permissions", &st::menuIconPermissions },
		{ "phone", &st::menuIconPhone },
		{ "photo", &st::menuIconPhoto },
		{ "pin", &st::menuIconPin },
		{ "premium", &st::menuIconPremium },
		{ "profile", &st::menuIconProfile },
		{ "qr", &st::menuIconQrCode },
		{ "reactions", &st::menuIconReactions },
		{ "remove", &st::menuIconRemove },
		{ "reply", &st::menuIconReply },
		{ "report", &st::menuIconReport },
		{ "restore", &st::menuIconRestore },
		{ "schedule", &st::menuIconSchedule },
		{ "search", &st::menuIconSearch },
		{ "select", &st::menuIconSelect },
		{ "send", &st::menuIconSend },
		{ "settings", &st::menuIconSettings },
		{ "share", &st::menuIconShare },
		{ "show", &st::menuIconUserShow },
		{ "silent", &st::menuIconSilent },
		{ "sound_off", &st::menuIconSoundOff },
		{ "sound_on", &st::menuIconSoundOn },
		{ "spoiler", &st::menuIconSpoiler },
		{ "star", &st::menuIconStar },
		{ "stats", &st::menuIconStats },
		{ "stickers", &st::menuIconStickers },
		{ "storage", &st::menuIconStorage },
		{ "timer", &st::menuIconTimer },
		{ "translate", &st::menuIconTranslate },
		{ "unmute", &st::menuIconUnmute },
		{ "unpin", &st::menuIconUnpin },
		{ "users", &st::menuIconGroups },
		{ "zoom_in", &st::menuIconZoomIn },
		{ "zoom_out", &st::menuIconZoomOut },
	};
	const auto i = icons.find(name);
	return (i != icons.end()) ? i->second : nullptr;
}

[[nodiscard]] QImage IconMask(
		const std::string &plugin,
		const std::string &name,
		QSize size) {
	const auto snapshot = Snapshot();
	const auto bytes = (snapshot && !name.empty())
		? snapshot->customIcon(plugin, name)
		: nullptr;
	if (!bytes) {
		return QImage();
	}
	struct Cached {
		std::shared_ptr<const fgpl::cbor::Bytes> bytes;
		QImage mask;
	};
	using Key = std::tuple<std::string, std::string, int, int, int>;
	static auto cache = std::map<Key, Cached>();
	const auto ratio = style::DevicePixelRatio();
	const auto key = Key{ plugin, name, size.width(), size.height(), ratio };
	if (const auto i = cache.find(key); i != cache.end() && i->second.bytes == bytes) {
		return i->second.mask;
	}
	auto image = QImage(size * ratio, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	const auto data = QByteArray(
		reinterpret_cast<const char*>(bytes->data()),
		qsizetype(bytes->size()));
	auto valid = true;
	{
		auto p = QPainter(&image);
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		auto svg = QSvgRenderer(data);
		if (svg.isValid()) {
			const auto box = svg.viewBoxF().isEmpty()
				? QSizeF(svg.defaultSize())
				: svg.viewBoxF().size();
			const auto fitted = box.isEmpty()
				? QSizeF(image.size())
				: box.scaled(QSizeF(image.size()), Qt::KeepAspectRatio);
			svg.render(&p, QRectF(
				QPointF(
					(image.width() - fitted.width()) / 2.,
					(image.height() - fitted.height()) / 2.),
				fitted));
		} else {
			const auto source = QImage::fromData(data).scaled(
				image.size(),
				Qt::KeepAspectRatio,
				Qt::SmoothTransformation);
			valid = !source.isNull();
			if (valid) {
				p.drawImage(
					(image.width() - source.width()) / 2,
					(image.height() - source.height()) / 2,
					source);
			}
		}
	}
	if (!valid) {
		image = QImage();
	} else {
		image.setDevicePixelRatio(ratio);
	}
	if (cache.size() > 256) {
		cache.clear();
	}
	cache[key] = Cached{ bytes, image };
	return image;
}

[[nodiscard]] bool HasCustomIcon(const std::string &plugin, const std::string &name) {
	return !IconMask(plugin, name, st::menuIconBot.size()).isNull();
}

void PaintPluginIcon(
		QPainter &p,
		QPoint position,
		int outerWidth,
		const std::string &plugin,
		const std::string &name,
		const style::icon &fallback,
		QColor color) {
	if (const auto builtin = BuiltinIcon(name)) {
		builtin->paint(p, position, outerWidth, color);
		return;
	}
	const auto mask = IconMask(plugin, name, fallback.size());
	if (mask.isNull()) {
		fallback.paint(p, position, outerWidth, color);
		return;
	}
	auto tinted = QImage(mask.size(), QImage::Format_ARGB32_Premultiplied);
	tinted.fill(color);
	{
		auto q = QPainter(&tinted);
		q.setCompositionMode(QPainter::CompositionMode_DestinationIn);
		q.drawImage(0, 0, mask);
	}
	tinted.setDevicePixelRatio(mask.devicePixelRatio());
	p.drawImage(style::rtlrect(QRect(position, fallback.size()), outerWidth).topLeft(), tinted);
}

class PluginMenuAction final : public Ui::Menu::Action {
public:
	PluginMenuAction(
		not_null<Ui::Menu::Menu*> menu,
		const style::Menu &st,
		not_null<QAction*> action,
		std::string plugin,
		std::string icon)
	: Action(menu, st, action, nullptr, nullptr)
	, _st(st)
	, _plugin(std::move(plugin))
	, _icon(std::move(icon)) {
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Action::paintEvent(e);
		auto p = QPainter(this);
		PaintPluginIcon(
			p,
			_st.itemIconPosition,
			width(),
			_plugin,
			_icon,
			st::menuIconBot,
			st::menuIconColor->c);
	}

private:
	const style::Menu &_st;
	const std::string _plugin;
	const std::string _icon;

};

void AddPluginAction(
		const Ui::Menu::MenuCallback &add,
		const Entry &entry,
		Fn<void()> handler) {
	if (const auto builtin = BuiltinIcon(entry.icon)) {
		add(entry.text, std::move(handler), builtin);
	} else if (HasCustomIcon(entry.plugin, entry.icon)) {
		add({
			.text = entry.text,
			.handler = handler,
			.icon = &st::menuIconBot,
			.make = [=](not_null<Ui::PopupMenu*> menu) {
				return base::unique_qptr<Ui::Menu::ItemBase>(
					base::make_unique_q<PluginMenuAction>(
						menu->menu(),
						menu->st().menu,
						Ui::Menu::CreateAction(
							menu->menu().get(),
							entry.text,
							Fn<void()>(handler)),
						entry.plugin,
						entry.icon));
			},
		});
	} else {
		add(entry.text, std::move(handler), &st::menuIconBot);
	}
}

void AddButtonIcon(
		not_null<Ui::RpWidget*> button,
		const style::SettingsButton &st,
		const std::string &plugin,
		const std::string &name,
		std::optional<int> left = std::nullopt) {
	const auto icon = Ui::CreateChild<Ui::RpWidget>(button.get());
	icon->setAttribute(Qt::WA_TransparentForMouseEvents);
	icon->resize(st::menuIconBot.size());
	button->sizeValue() | rpl::on_next([=](QSize size) {
		icon->moveToLeft(left.value_or(st.iconLeft), (size.height() - icon->height()) / 2);
	}, icon->lifetime());
	icon->paintOn([=](QPainter &p) {
		PaintPluginIcon(p, QPoint(), icon->width(), plugin, name, st::menuIconBot, st::menuIconColor->c);
	});
	icon->show();
}

[[nodiscard]] std::vector<Entry> EntriesFor(
		not_null<Main::Session*> session,
		const char *surface,
		PeerData *peer = nullptr) {
	auto result = std::vector<Entry>();
	const auto snapshot = Snapshot();
	if (!snapshot) {
		return result;
	} else if (peer
		&& (FreshSecret::IsSecretPeerId(peer->id)
			|| int64_t(peer->id.value) == fgpl::api::kServiceChat)) {
		return result;
	}
	for (const auto &plugin : snapshot->contributions(AccountOf(session), Language())) {
		if (peer) {
			const auto grant = snapshot->grantsOf(plugin.id);
			if (grant
				&& grant->chats
				&& !grant->chats->contains(int64_t(peer->id.value))) {
				continue;
			}
		}
		for (const auto &menu : plugin.menus) {
			if (menu.surface != surface) {
				continue;
			}
			result.push_back({
				plugin.id,
				menu.id,
				tr::fresh_PluginBadge(
					tr::now,
					lt_title,
					Utf8(menu.title),
					lt_plugin,
					Utf8(plugin.name)),
				menu.icon,
			});
		}
	}
	return result;
}

void Send(
		not_null<Main::Session*> session,
		const std::string &plugin,
		cbor::Value event) {
	if (!Running()) {
		::Ui::Toast::Show(tr::fresh_PluginNotRunning(tr::now));
		return;
	}
	const auto account = AccountOf(session);
	Post([=, event = std::move(event)](not_null<fgpl::PluginManager*> manager) {
		if (!manager->send(plugin, account, event)) {
			crl::on_main([] {
				::Ui::Toast::Show(tr::fresh_PluginNotRunning(tr::now));
			});
		}
	}, false);
}

[[nodiscard]] cbor::Value ClickEvent(
		const char *surface,
		const std::string &menuId,
		PeerData *peer,
		HistoryItem *item,
		MsgId topicRootId = MsgId(),
		MsgId replyToId = MsgId()) {
	auto map = cbor::Map();
	map.push_back({ "type", cbor::Value::FromText("menu.click") });
	map.push_back({ "surface", cbor::Value::FromText(surface) });
	map.push_back({ "id", cbor::Value::FromText(menuId) });
	if (peer) {
		map.push_back({ "chat", Number(int64_t(peer->id.value)) });
		map.push_back({ "secret", Flag(FreshSecret::IsSecretPeerId(peer->id)) });
	}
	if (topicRootId) {
		map.push_back({ "topic", Number(topicRootId.bare) });
	}
	if (replyToId) {
		map.push_back({ "reply_to", Number(replyToId.bare) });
	}
	if (item) {
		if (const auto topic = item->topicRootId()) {
			map.push_back({ "topic", Number(topic.bare) });
		}
		map.push_back({ "message", Number(item->id.bare) });
		map.push_back({ "from", Number(int64_t(item->from()->id.value)) });
		map.push_back({ "text", Text(item->originalText().text) });
	}
	return cbor::Value::FromMap(std::move(map));
}

[[nodiscard]] const cbor::Value *PageOf(
		const std::string &id,
		const std::string &account) {
	const auto &pages = GetPages().pages;
	const auto i = pages.find({ id, account });
	return (i != pages.end()) ? &i->second : nullptr;
}

[[nodiscard]] QString TextOf(const cbor::Value &item, const char *name) {
	const auto field = item.find(name);
	return (field && field->asText()) ? Utf8(*field->asText()) : QString();
}

void SendSetting(
		not_null<Main::Session*> session,
		const std::string &plugin,
		const char *type,
		const std::string &itemId,
		std::optional<cbor::Value> value) {
	auto map = cbor::Map();
	map.push_back({ "type", cbor::Value::FromText(type) });
	map.push_back({ "id", cbor::Value::FromText(itemId) });
	if (value) {
		map.push_back({ "value", std::move(*value) });
	}
	Send(session, plugin, cbor::Value::FromMap(std::move(map)));
}

struct Page {
	QString structure;
	std::vector<Fn<void(const cbor::Value &item)>> update;
	std::vector<Fn<void()>> flush;
	rpl::lifetime lifetime;

	void apply(const cbor::Value &page) {
		const auto items = page.find("items");
		const auto list = (items && items->asArray()) ? *items->asArray() : cbor::Array();
		for (auto i = size_t(0); i != list.size() && i != update.size(); ++i) {
			if (update[i]) {
				update[i](list[i]);
			}
		}
	}
	void flushAll() {
		for (const auto &callback : flush) {
			callback();
		}
	}
};

[[nodiscard]] bool FlagOf(const cbor::Value &item, const char *name) {
	const auto field = item.find(name);
	return field && field->asBool().value_or(false);
}

[[nodiscard]] int64_t IntOf(const cbor::Value &item, const char *name) {
	const auto field = item.find(name);
	return (field && field->asInt()) ? *field->asInt() : 0;
}

[[nodiscard]] QString StructureOf(const cbor::Value &page, bool withButtons = true) {
	const auto items = page.find("items");
	auto result = QStringList();
	for (const auto &item : (items && items->asArray()) ? *items->asArray() : cbor::Array()) {
		const auto type = TextOf(item, "type");
		result.push_back(type
			+ '/' + TextOf(item, "id")
			+ (FlagOf(item, "multiline") ? u"/m"_q : QString())
			+ (FlagOf(item, "password") ? u"/p"_q : QString())
			+ (TextOf(item, "placeholder").isEmpty() ? QString() : u"/ph"_q)
			+ '/' + TextOf(item, "icon")
			+ '/' + TextOf(item, "style")
			+ ((type == u"slider"_q)
				? (u"/%1/%2/%3"_q
					.arg(IntOf(item, "min"))
					.arg(IntOf(item, "max"))
					.arg(IntOf(item, "step")))
				: QString()));
	}
	const auto buttons = withButtons ? page.find("buttons") : nullptr;
	for (const auto &button : (buttons && buttons->asArray()) ? *buttons->asArray() : cbor::Array()) {
		result.push_back(u"buttons/"_q + TextOf(button, "id") + '/' + TextOf(button, "label")
			+ '/' + TextOf(button, "style"));
	}
	return result.join('\n');
}

[[nodiscard]] QString ChatName(base::weak_ptr<Main::Session> weak, int64_t id) {
	const auto session = weak.get();
	const auto peer = (session && id)
		? session->data().peerLoaded(PeerId(uint64(id)))
		: nullptr;
	return peer
		? peer->name()
		: id
		? (u"#"_q + QString::number(id))
		: tr::fresh_PluginChatNone(tr::now);
}

void AddProgress(
		not_null<Ui::VerticalLayout*> content,
		rpl::producer<int> value) {
	const auto bar = content->add(
		object_ptr<Ui::RpWidget>(content),
		st::boxRowPadding);
	bar->resize(bar->width(), st::lineWidth * 4);
	const auto current = bar->lifetime().make_state<rpl::variable<int>>(std::move(value));
	current->changes() | rpl::on_next([=] {
		bar->update();
	}, bar->lifetime());
	bar->paintOn([=](QPainter &p) {
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = bar->height() / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(bar->rect(), radius, radius);
		const auto percent = current->current();
		if (percent > 0) {
			p.setBrush(st::windowBgActive);
			p.drawRoundedRect(
				QRect(0, 0, bar->width() * std::min(percent, 100) / 100, bar->height()),
				radius,
				radius);
		}
	});
	Ui::AddSkip(content);
}

[[nodiscard]] const style::SettingsButton &RowStyle(
		const QString &icon,
		const QString &style) {
	const auto withIcon = !icon.isEmpty();
	return (style == u"danger"_q)
		? (withIcon ? st::settingsAttentionButtonWithIcon : st::settingsAttentionButton)
		: (style == u"primary"_q)
		? (withIcon ? st::settingsButtonLight : st::settingsButtonLightNoIcon)
		: (withIcon ? st::settingsButton : st::settingsButtonNoIcon);
}

[[nodiscard]] style::margins TextPadding(const style::SettingsButton &st) {
	return style::margins(st.padding.left(), 0, st.padding.right(), 0);
}

[[nodiscard]] std::shared_ptr<Page> BuildSettingsContent(
		not_null<Ui::VerticalLayout*> content,
		not_null<Window::SessionController*> controller,
		base::weak_ptr<Main::Session> weak,
		const std::string &plugin,
		const cbor::Value &page,
		bool dialog) {
	enum class Last {
		None,
		Row,
		Block,
		Section,
	};
	auto result = std::make_shared<Page>();
	result->structure = StructureOf(page, !dialog);
	const auto raw = result.get();
	const auto change = dialog ? "dialog.change" : "settings.change";
	const auto click = dialog ? "dialog.click" : "settings.click";
	const auto items = page.find("items");
	auto last = Last::None;
	const auto space = [&](Last next) {
		if (last == Last::None) {
			Ui::AddSkip(content, st::defaultVerticalListSkip);
		} else if (last != next || next == Last::Block) {
			Ui::AddSkip(content);
		}
		last = next;
	};
	const auto addRow = [&](
			rpl::producer<QString> text,
			const QString &icon,
			const QString &style,
			std::optional<rpl::producer<QString>> right = std::nullopt) {
		space(Last::Row);
		const auto &st = RowStyle(icon, style);
		const auto name = icon.toStdString();
		const auto builtin = BuiltinIcon(name);
		auto descriptor = Settings::IconDescriptor{ builtin };
		const auto button = right
			? Settings::AddButtonWithLabel(
				content,
				std::move(text),
				std::move(*right),
				st,
				std::move(descriptor))
			: Settings::AddButtonWithIcon(
				content,
				std::move(text),
				st,
				std::move(descriptor));
		if (!icon.isEmpty() && !builtin) {
			AddButtonIcon(button, st, plugin, name);
		}
		return button;
	};
	const auto addHint = [&](
			const cbor::Value &item,
			const style::SettingsButton &st) -> Fn<void(const cbor::Value &)> {
		static const auto interactive = std::set<QString>{
			u"toggle"_q, u"button"_q, u"input"_q, u"slider"_q, u"chat"_q, u"select"_q,
		};
		if (!interactive.contains(TextOf(item, "type"))) {
			return nullptr;
		}
		const auto wrap = content->add(
			object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
				content,
				object_ptr<Ui::FlatLabel>(content, TextOf(item, "hint"), st::boxDividerLabel),
				TextPadding(st) + style::margins(0, 0, 0, st::defaultVerticalListSkip)));
		wrap->toggle(!TextOf(item, "hint").isEmpty(), anim::type::instant);
		return crl::guard(wrap, [=](const cbor::Value &now) {
			const auto text = TextOf(now, "hint");
			wrap->entity()->setText(text);
			wrap->toggle(!text.isEmpty(), anim::type::instant);
		});
	};
	const auto withHint = [](
			Fn<void(const cbor::Value &)> main,
			Fn<void(const cbor::Value &)> hint) -> Fn<void(const cbor::Value &)> {
		if (!hint) {
			return main;
		}
		return [=](const cbor::Value &now) {
			if (main) {
				main(now);
			}
			hint(now);
		};
	};
	for (const auto &item : (items && items->asArray()) ? *items->asArray() : cbor::Array()) {
		const auto type = TextOf(item, "type");
		const auto itemId = TextOf(item, "id").toStdString();
		const auto icon = TextOf(item, "icon").isEmpty()
			? ((type == u"link"_q)
				? u"link"_q
				: (type == u"chat"_q)
				? u"chats"_q
				: QString())
			: TextOf(item, "icon");
		const auto &rowSt = RowStyle(icon, TextOf(item, "style"));
		const auto label = raw->lifetime.make_state<rpl::variable<QString>>(
			TextOf(item, "label"));
		const auto send = [=](const char *kind, std::optional<cbor::Value> value) {
			if (const auto strong = weak.get()) {
				SendSetting(strong, plugin, kind, itemId, std::move(value));
			}
		};
		auto update = Fn<void(const cbor::Value &)>();
		if (type == u"text"_q) {
			space(Last::Block);
			const auto text = content->add(
				object_ptr<Ui::FlatLabel>(content, TextOf(item, "text"), st::boxLabel),
				TextPadding(st::settingsButtonNoIcon));
			text->setSelectable(true);
			update = crl::guard(text, [=](const cbor::Value &now) {
				text->setText(TextOf(now, "text"));
			});
		} else if (type == u"header"_q) {
			if (last != Last::None) {
				Ui::AddSkip(content);
			}
			last = Last::Section;
			*label = TextOf(item, "text");
			Ui::AddSubsectionTitle(content, label->value());
			update = [=](const cbor::Value &now) {
				*label = TextOf(now, "text");
			};
		} else if (type == u"divider"_q) {
			if (last != Last::None) {
				Ui::AddSkip(content);
				Ui::AddDivider(content);
			}
			last = Last::Section;
		} else if (type == u"link"_q) {
			const auto url = raw->lifetime.make_state<QString>(TextOf(item, "url"));
			const auto host = raw->lifetime.make_state<rpl::variable<QString>>(
				QUrl(*url).host());
			const auto button = addRow(
				label->value(),
				icon,
				QString(),
				host->value());
			button->addClickHandler([=] {
				UrlClickHandler::Open(*url);
			});
			update = crl::guard(button, [=](const cbor::Value &now) {
				*label = TextOf(now, "label");
				*url = TextOf(now, "url");
				*host = QUrl(*url).host();
			});
		} else if (type == u"progress"_q) {
			space(Last::Block);
			const auto percent = raw->lifetime.make_state<rpl::variable<int>>(
				int(IntOf(item, "value")));
			const auto title = Settings::AddButtonWithLabel(
				content,
				label->value(),
				percent->value() | rpl::map([](int value) {
					return (value < 0) ? QString() : (QString::number(value) + '%');
				}),
				st::settingsButtonNoIcon);
			title->setAttribute(Qt::WA_TransparentForMouseEvents);
			AddProgress(content, percent->value());
			update = [=](const cbor::Value &now) {
				*label = TextOf(now, "label");
				*percent = int(IntOf(now, "value"));
			};
		} else if (type == u"toggle"_q) {
			const auto button = addRow(label->value(), icon, QString());
			const auto on = button->lifetime().make_state<rpl::variable<bool>>(
				FlagOf(item, "value"));
			button->toggleOn(on->value(), true);
			button->setDisabled(FlagOf(item, "disabled"));
			button->addClickHandler([=] {
				*on = !on->current();
				send(change, Flag(on->current()));
			});
			update = crl::guard(button, [=](const cbor::Value &now) {
				*label = TextOf(now, "label");
				*on = FlagOf(now, "value");
				button->setDisabled(FlagOf(now, "disabled"));
			});
		} else if (type == u"button"_q) {
			const auto button = addRow(label->value(), icon, TextOf(item, "style"));
			button->setDisabled(FlagOf(item, "disabled"));
			button->addClickHandler([=] {
				send(click, std::nullopt);
			});
			update = crl::guard(button, [=](const cbor::Value &now) {
				*label = TextOf(now, "label");
				button->setDisabled(FlagOf(now, "disabled"));
			});
		} else if (type == u"chat"_q) {
			const auto chosen = raw->lifetime.make_state<int64_t>(IntOf(item, "value"));
			const auto name = raw->lifetime.make_state<rpl::variable<QString>>(
				ChatName(weak, *chosen));
			const auto button = addRow(
				label->value(),
				icon,
				QString(),
				name->value());
			button->setDisabled(FlagOf(item, "disabled"));
			button->addClickHandler([=] {
				Window::ShowChooseRecipientBox(
					controller,
					crl::guard(button, [=](not_null<Data::Thread*> thread) {
						const auto peer = thread->peer();
						if (FreshSecret::IsSecretPeerId(peer->id)) {
							return false;
						}
						*chosen = int64_t(peer->id.value);
						*name = peer->name();
						send(change, Number(*chosen));
						return true;
					}),
					label->value());
			});
			update = crl::guard(button, [=](const cbor::Value &now) {
				*label = TextOf(now, "label");
				*chosen = IntOf(now, "value");
				*name = ChatName(weak, *chosen);
				button->setDisabled(FlagOf(now, "disabled"));
			});
		} else if (type == u"slider"_q) {
			space(Last::Block);
			const auto min = IntOf(item, "min");
			const auto step = std::max<int64_t>(IntOf(item, "step"), 1);
			const auto count = std::max(int((IntOf(item, "max") - min) / step) + 1, 2);
			const auto title = Settings::AddButtonWithIcon(
				content,
				label->value(),
				st::settingsButtonNoIcon);
			title->setAttribute(Qt::WA_TransparentForMouseEvents);
			auto made = Settings::MakeSliderWithLabel(
				content,
				st::autoDownloadLimitSlider,
				st::settingsScaleLabel,
				st::normalFont->spacew * 2,
				st::settingsScaleLabel.style.font->width(QString::number(IntOf(item, "max"))),
				true);
			const auto slider = made.slider;
			const auto value = made.label;
			content->add(std::move(made.widget), st::recentStickersLimitPadding);
			const auto show = [=](int64_t number) {
				value->setText(QString::number(number));
			};
			show(IntOf(item, "value"));
			slider->setPseudoDiscrete(
				count,
				[=](int index) { return int(min + index * step); },
				int(IntOf(item, "value")),
				show,
				[=](int number) {
					show(number);
					send(change, Number(number));
				});
			slider->setDisabled(FlagOf(item, "disabled"));
			update = crl::guard(slider, [=](const cbor::Value &now) {
				*label = TextOf(now, "label");
				if (!slider->isChanging()) {
					const auto number = IntOf(now, "value");
					slider->setValue(count > 1 ? double(number - min) / (step * (count - 1)) : 0.);
					show(number);
				}
				slider->setDisabled(FlagOf(now, "disabled"));
			});
		} else if (type == u"input"_q) {
			space(Last::Block);
			const auto max = int(std::max<int64_t>(IntOf(item, "max"), 1));
			auto getText = Fn<QString()>();
			auto setText = Fn<void(const QString &)>();
			auto focused = Fn<bool()>();
			auto setEnabled = Fn<void(bool)>();
			const auto pending = raw->lifetime.make_state<bool>(false);
			const auto suppress = raw->lifetime.make_state<bool>(false);
			const auto placeholder = raw->lifetime.make_state<rpl::variable<QString>>(
				TextOf(item, "placeholder"));
			if (!placeholder->current().isEmpty()) {
				content->add(
					object_ptr<Ui::FlatLabel>(content, label->value(), st::boxDividerLabel),
					st::boxRowPadding);
			}
			auto fieldPlaceholder = rpl::combine(
				label->value(),
				placeholder->value()
			) | rpl::map([](const QString &label, const QString &placeholder) {
				return placeholder.isEmpty() ? label : placeholder;
			});
			const auto sendNow = crl::guard(content, [=] {
				if (*pending) {
					*pending = false;
					send(change, Text(getText()));
				}
			});
			const auto timer = raw->lifetime.make_state<base::Timer>(sendNow);
			const auto edited = [=] {
				if (*suppress) {
					return;
				}
				*pending = true;
				timer->callOnce(600);
			};
			if (FlagOf(item, "password")) {
				const auto wrap = content->add(
					object_ptr<Ui::RpWidget>(content),
					st::boxRowPadding);
				const auto field = Ui::CreateChild<Ui::PasswordInput>(
					wrap,
					st::defaultInputField,
					std::move(fieldPlaceholder),
					TextOf(item, "value"));
				field->show();
				field->heightValue() | rpl::on_next([=](int height) {
					wrap->resize(wrap->width(), height);
				}, field->lifetime());
				wrap->widthValue() | rpl::on_next([=](int width) {
					field->resizeToWidth(width);
					field->moveToLeft(0, 0);
				}, field->lifetime());
				field->setMaxLength(max);
				QObject::connect(field, &Ui::MaskedInputField::changed, edited);
				QObject::connect(field, &Ui::MaskedInputField::blurred, sendNow);
				getText = [=] { return field->getLastText(); };
				setText = [=](const QString &text) { field->setText(text); };
				focused = [=] { return field->hasFocus(); };
				setEnabled = [=](bool enabled) { field->setEnabled(enabled); };
			} else {
				const auto multiline = FlagOf(item, "multiline");
				const auto field = content->add(
					object_ptr<Ui::InputField>(
						content,
						multiline ? st::newGroupDescription : st::defaultInputField,
						multiline
							? Ui::InputField::Mode::MultiLine
							: Ui::InputField::Mode::SingleLine,
						std::move(fieldPlaceholder),
						TextOf(item, "value")),
					st::boxRowPadding);
				field->setMaxLength(max);
				field->changes() | rpl::on_next(edited, field->lifetime());
				field->focusedChanges(
				) | rpl::filter(!rpl::mappers::_1) | rpl::on_next([=] {
					sendNow();
				}, field->lifetime());
				getText = [=] { return field->getLastText(); };
				setText = [=](const QString &text) {
					*suppress = true;
					field->setText(text);
					*suppress = false;
				};
				focused = [=] { return field->hasFocus(); };
				setEnabled = [=](bool enabled) { field->setEnabled(enabled); };
			}
			setEnabled(!FlagOf(item, "disabled"));
			raw->flush.push_back(sendNow);
			Ui::AddSkip(content, st::defaultVerticalListSkip);
			update = crl::guard(content, [=](const cbor::Value &now) {
				*label = TextOf(now, "label");
				*placeholder = TextOf(now, "placeholder");
				const auto value = TextOf(now, "value");
				if (!focused() && !*pending && getText() != value) {
					setText(value);
				}
				setEnabled(!FlagOf(now, "disabled"));
			});
		} else if (type == u"select"_q) {
			struct Choice {
				std::vector<std::string> values;
				std::vector<QString> labels;
				int selected = 0;
			};
			const auto choice = std::make_shared<Choice>();
			const auto current = raw->lifetime.make_state<rpl::variable<QString>>();
			const auto read = [=](const cbor::Value &now) {
				choice->values.clear();
				choice->labels.clear();
				choice->selected = 0;
				const auto chosen = TextOf(now, "value").toStdString();
				const auto options = now.find("options");
				for (const auto &option : (options && options->asArray()) ? *options->asArray() : cbor::Array()) {
					const auto value = option.find("value");
					if (!value || !value->asText()) {
						continue;
					}
					if (*value->asText() == chosen) {
						choice->selected = int(choice->values.size());
					}
					choice->values.push_back(*value->asText());
					choice->labels.push_back(TextOf(option, "label"));
				}
				*current = choice->labels.empty()
					? QString()
					: choice->labels[choice->selected];
			};
			read(item);
			if (choice->labels.empty()) {
				result->update.push_back(nullptr);
				continue;
			}
			const auto button = addRow(label->value(), icon, QString(), current->value());
			button->setDisabled(FlagOf(item, "disabled"));
			button->addClickHandler([=] {
				const auto values = choice->values;
				const auto labels = choice->labels;
				controller->show(Box(SingleChoiceBox, SingleChoiceBoxArgs{
					.title = label->value(),
					.options = labels,
					.initialSelection = choice->selected,
					.callback = crl::guard(button, [=](int index) {
						if (index < 0 || index >= int(values.size())) {
							return;
						}
						const auto now = ranges::find(choice->values, values[index]);
						choice->selected = int(now - begin(choice->values));
						*current = labels[index];
						send(change, cbor::Value::FromText(values[index]));
					}),
				}));
			});
			update = crl::guard(button, [=](const cbor::Value &now) {
				*label = TextOf(now, "label");
				read(now);
				button->setDisabled(FlagOf(now, "disabled"));
			});
		}
		result->update.push_back(withHint(std::move(update), addHint(item, rowSt)));
	}
	const auto buttons = page.find("buttons");
	if (!dialog && buttons && buttons->asArray() && !buttons->asArray()->empty()) {
		if (last != Last::None) {
			Ui::AddSkip(content);
			Ui::AddDivider(content);
		}
		last = Last::None;
		for (const auto &button : *buttons->asArray()) {
			const auto buttonId = TextOf(button, "id").toStdString();
			addRow(rpl::single(TextOf(button, "label")), QString(), TextOf(button, "style"))->addClickHandler([=] {
				if (const auto strong = weak.get()) {
					SendSetting(strong, plugin, click, buttonId, std::nullopt);
				}
			});
		}
	}
	Ui::AddSkip(content);
	return result;
}

void ShowPage(
		not_null<Ui::VerticalLayout*> content,
		not_null<Window::SessionController*> controller,
		base::weak_ptr<Main::Session> weak,
		const std::string &plugin,
		const cbor::Value *page,
		bool dialog,
		std::shared_ptr<Page> &shown) {
	if (!page) {
		if (shown) {
			shown->flushAll();
		}
		shown = nullptr;
		content->clear();
		return;
	} else if (shown && shown->structure == StructureOf(*page, !dialog)) {
		shown->apply(*page);
		return;
	}
	if (shown) {
		shown->flushAll();
	}
	content->clear();
	shown = BuildSettingsContent(content, controller, weak, plugin, *page, dialog);
}

void AddSourceRow(
		not_null<Ui::GenericBox*> box,
		const QString &pluginName,
		const QString &suffix) {
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::fresh_PluginSource(tr::now, lt_plugin, pluginName) + suffix,
			st::boxDividerLabel),
		TextPadding(st::settingsButtonNoIcon)
			+ style::margins(0, st::defaultVerticalListSkip, 0, 0));
}

void FillSettingsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		std::string plugin,
		std::string account,
		QString pluginName) {
	box->setWidth(st::boxWideWidth);
	const auto title = box->lifetime().make_state<rpl::variable<QString>>();
	box->setTitle(title->value() | rpl::map([=](const QString &text) {
		return text.isEmpty() ? pluginName : text;
	}));
	AddSourceRow(box, pluginName, QString());
	const auto content = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto shown = box->lifetime().make_state<std::shared_ptr<Page>>();
	const auto weak = base::make_weak(&controller->session());
	const auto waited = box->lifetime().make_state<bool>(false);
	const auto rebuild = [=] {
		const auto page = PageOf(plugin, account);
		*title = page ? TextOf(*page, "title") : QString();
		ShowPage(content, controller, weak, plugin, page, false, *shown);
		if (!page) {
			content->add(
				object_ptr<Ui::FlatLabel>(
					content,
					(*waited
						? tr::fresh_PluginSettingsUnavailable
						: tr::fresh_PluginSettingsLoading)(tr::now),
					st::boxDividerLabel),
				st::boxRowPadding);
		}
	};
	rebuild();
	if (!PageOf(plugin, account)) {
		const auto timer = box->lifetime().make_state<base::Timer>([=] {
			*waited = true;
			rebuild();
		});
		timer->callOnce(kSettingsWaitMs);
	}
	GetPages().changes.events(
	) | rpl::filter([=](const std::pair<std::string, std::string> &key) {
		return key.first == plugin && key.second == account;
	}) | rpl::on_next([=] {
		Ui::PostponeCall(content, rebuild);
	}, content->lifetime());
	box->boxClosing() | rpl::on_next([=] {
		if (*shown) {
			(*shown)->flushAll();
		}
	}, box->lifetime());
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

[[nodiscard]] bool IsProblemLine(const fgpl::LogLine &line) {
	static const auto error = QRegularExpression(
		u"^\\s*(Traceback \\(most recent call last\\)|[\\w.]*(Error|Exception)\\b)"_q);
	const auto text = Utf8(line.text);
	return (line.fromApp
			&& (text.startsWith(u"failure:"_q) || text.startsWith(u"rolled back"_q)))
		|| error.match(text).hasMatch();
}

[[nodiscard]] QString LogText(const fgpl::LogLine &line) {
	return QDateTime::fromMSecsSinceEpoch(line.unixMs).toString(u"HH:mm:ss"_q)
		+ u"  "_q
		+ Utf8(line.text);
}

struct LogRow {
	QString key;
	QString text;
	const style::FlatLabel *st = nullptr;
};

struct LogView {
	std::optional<QString> last;
	bool empty = false;
	bool filled = false;
};

void ShowLogRows(
		not_null<Ui::GenericBox*> box,
		not_null<Ui::VerticalLayout*> lines,
		not_null<LogView*> view,
		const std::vector<LogRow> &rows,
		bool reset,
		const QString &emptyText) {
	const auto atBottom = !view->filled
		|| (box->scrollTop() + box->scrollHeight()
			>= box->verticalLayout()->height() - st::defaultVerticalListSkip);
	auto from = size_t(0);
	if (!view->filled || (view->empty != rows.empty())) {
		reset = true;
	} else if (rows.empty() && !reset) {
		return;
	} else if (!reset) {
		const auto i = std::find_if(rows.rbegin(), rows.rend(), [&](const LogRow &row) {
			return view->last && (row.key == *view->last);
		});
		if (i == rows.rend()) {
			reset = true;
		} else {
			from = size_t(rows.rend() - i);
			if (from == rows.size()) {
				return;
			}
		}
	}
	if (reset) {
		lines->clear();
		from = 0;
	}
	for (auto i = from; i != rows.size(); ++i) {
		lines->add(
			object_ptr<Ui::FlatLabel>(lines, rows[i].text, *rows[i].st),
			st::boxRowPadding)->setSelectable(true);
	}
	view->empty = rows.empty();
	if (view->empty) {
		lines->add(
			object_ptr<Ui::FlatLabel>(lines, emptyText, st::boxDividerLabel),
			st::boxRowPadding);
	}
	view->last = rows.empty() ? std::nullopt : std::make_optional(rows.back().key);
	view->filled = true;
	if (atBottom) {
		crl::on_main(box, [=] {
			box->scrollToY(QWIDGETSIZE_MAX);
		});
	}
}

void FillLogBox(
		not_null<Ui::GenericBox*> box,
		std::string plugin,
		QString name) {
	box->setTitle(rpl::single(tr::fresh_PluginBadge(
		tr::now,
		lt_title,
		tr::fresh_PluginLog(tr::now),
		lt_plugin,
		name)));
	box->setWidth(st::boxWideWidth);
	const auto onlyProblems = box->lifetime().make_state<rpl::variable<bool>>(false);
	const auto filter = Settings::AddButtonWithIcon(
		box->verticalLayout(),
		tr::fresh_PluginLogOnlyProblems(),
		st::settingsButtonNoIcon);
	filter->toggleOn(rpl::single(false));
	Ui::AddSkip(box->verticalLayout());
	const auto lines = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto view = box->lifetime().make_state<LogView>();
	const auto show = [=](bool reset) {
		const auto snapshot = Snapshot();
		const auto all = snapshot
			? snapshot->logLines(plugin)
			: std::vector<fgpl::LogLine>();
		const auto problems = onlyProblems->current();
		auto rows = std::vector<LogRow>();
		for (const auto &line : all) {
			const auto problem = IsProblemLine(line);
			if (problems && !problem) {
				continue;
			}
			rows.push_back({
				.key = QString::number(line.unixMs) + ' ' + Utf8(line.text),
				.text = LogText(line),
				.st = problem
					? &st::pluginDangerHigh
					: line.fromApp
					? &st::boxDividerLabel
					: &st::boxLabel,
			});
		}
		ShowLogRows(
			box,
			lines,
			view,
			rows,
			reset,
			problems
				? tr::fresh_PluginLogNoProblems(tr::now)
				: tr::fresh_PluginLogEmpty(tr::now));
	};
	const auto rebuild = [=] { show(false); };
	filter->toggledValue(
	) | rpl::on_next([=](bool value) {
		*onlyProblems = value;
		show(true);
	}, filter->lifetime());
	const auto timer = box->lifetime().make_state<base::Timer>(rebuild);
	timer->callEach(1000);

	box->addLeftButton(tr::fresh_PluginLogClear(), [=] {
		Post([=](not_null<fgpl::PluginManager*> manager) {
			manager->clearLog(plugin);
		}, false);
	});
	box->addButton(tr::fresh_PluginLogCopy(), [=] {
		const auto snapshot = Snapshot();
		auto text = QStringList();
		for (const auto &line : snapshot ? snapshot->logLines(plugin) : std::vector<fgpl::LogLine>()) {
			if (!onlyProblems->current() || IsProblemLine(line)) {
				text.push_back(LogText(line));
			}
		}
		QGuiApplication::clipboard()->setText(text.join('\n'));
		box->showToast(tr::lng_text_copied(tr::now));
	});
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

[[nodiscard]] QString ContributionSummary(const fgpl::Contribution &entry) {
	auto parts = QStringList();
	if (!entry.menus.empty()) {
		parts.push_back(tr::fresh_PluginContribMenus(tr::now));
	}
	if (!entry.commands.empty()) {
		parts.push_back(tr::fresh_PluginContribCommands(tr::now));
	}
	if (entry.settingsPage) {
		parts.push_back(tr::fresh_PluginContribSettings(tr::now));
	}
	return parts.isEmpty()
		? QString()
		: tr::fresh_PluginContributes(tr::now, lt_list, parts.join(u", "_q));
}

[[nodiscard]] bool UsesNetwork(const fgpl::PluginInfo &info) {
	const auto snapshot = Snapshot();
	const auto grant = snapshot ? snapshot->grantsOf(info.id) : std::nullopt;
	if (!grant) {
		return false;
	}
	for (const auto permission : { "net.https", "net.tcp", "net.udp" }) {
		if (grant->granted.contains(permission)) {
			return true;
		}
	}
	return false;
}

void FillPermissionsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		std::string plugin) {
	const auto snapshot = Snapshot();
	const auto parsed = snapshot ? snapshot->manifest(plugin) : nullptr;
	if (!parsed) {
		crl::on_main(box, [=] { box->closeBox(); });
		return;
	}
	const auto account = AccountOf(&controller->session());
	box->setTitle(rpl::single(tr::fresh_PluginBadge(
		tr::now,
		lt_title,
		tr::fresh_PluginPermissions(tr::now),
		lt_plugin,
		Utf8(parsed->name.get(Language())))));
	box->setWidth(st::boxWideWidth);
	const auto decision = AddPermissionsEditor(
		box,
		controller,
		*parsed,
		snapshot->grantsOf(plugin),
		{});
	box->addButton(tr::fresh_PluginApply(), [=] {
		const auto chosen = decision();
		if (!chosen) {
			return;
		}
		Post([=, chosen = *chosen](not_null<fgpl::PluginManager*> manager) {
			manager->grant(plugin, account, chosen);
			manager->startEnabled(account);
		});
		controller->showToast(tr::fresh_PluginPermissionsSaved(tr::now));
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

[[nodiscard]] QString AboutText(
		const fgpl::ManagerSnapshot &snapshot,
		const fgpl::PluginInfo &info,
		const fgpl::Manifest *parsed,
		const std::string &account) {
	auto lines = QStringList();
	if (parsed) {
		for (const auto &entry : snapshot.contributions(account, Language())) {
			if (entry.id == info.id) {
				if (const auto text = ContributionSummary(entry); !text.isEmpty()) {
					lines.push_back(text);
				}
			}
		}
		if (!parsed->messagesFrom.empty()) {
			lines.push_back(AcceptsFrom(parsed->messagesFrom));
		}
	}
	lines.push_back(tr::fresh_PluginStats(
		tr::now,
		lt_cpu,
		QString::number(info.stats.cpuMs / 1000., 'f', 1),
		lt_memory,
		Ui::FormatSizeText(qint64(info.stats.memoryBytes)),
		lt_received,
		Ui::FormatSizeText(qint64(info.stats.netReceived)),
		lt_sent,
		Ui::FormatSizeText(qint64(info.stats.netSent))));
	if (info.stats.unloads) {
		lines.push_back(tr::fresh_PluginUnloads(
			tr::now,
			lt_times,
			QString::number(info.stats.unloads)));
	}
	lines.push_back(tr::fresh_PluginId(tr::now, lt_id, Utf8(info.id)));
	return lines.join('\n');
}

void BuildDetails(
		not_null<Ui::GenericBox*> box,
		not_null<Ui::VerticalLayout*> content,
		not_null<Window::SessionController*> controller,
		const std::string &plugin) {
	const auto snapshot = Snapshot();
	const auto info = snapshot ? snapshot->plugin(plugin) : nullptr;
	if (!info) {
		crl::on_main(box, [=] { box->closeBox(); });
		return;
	}
	const auto parsed = snapshot->manifest(plugin);
	const auto account = AccountOf(&controller->session());
	const auto broken = !info->problem.empty();
	const auto name = parsed
		? Utf8(parsed->name.get(Language()))
		: info->name.empty()
		? Utf8(info->id)
		: Utf8(info->name);
	box->setTitle(tr::fresh_PluginDetails());

	const auto label = [&](const QString &text, const style::FlatLabel &st) {
		if (!text.isEmpty()) {
			content->add(
				object_ptr<Ui::FlatLabel>(content, text, st),
				st::boxRowPadding);
		}
	};
	const auto icon = snapshot->icon(plugin);
	AddPluginHeader(
		content,
		parsed
			? HeaderFor(
				*parsed,
				info->publisher,
				info->signedPackage,
				icon
					? QByteArray(
						reinterpret_cast<const char*>(icon->data()),
						qsizetype(icon->size()))
					: QByteArray())
			: PluginHeader{
				.name = name,
				.version = Utf8(info->version),
				.publisher = Utf8(info->publisher),
				.signedPackage = info->signedPackage,
			});
	Ui::AddSkip(content);
	if (broken) {
		label(tr::fresh_PluginBroken(tr::now, lt_reason, Utf8(info->problem)), st::pluginDangerHigh);
	} else if (!info->disabledReason.empty()) {
		label(tr::fresh_PluginStatusDisabled(tr::now, lt_reason, Utf8(info->disabledReason)), st::pluginDangerHigh);
	}
	if (info->pendingUpdate) {
		label(tr::fresh_PluginPending(tr::now, lt_version, Utf8(info->version)), st::pluginDangerMedium);
	}
	if (info->outdated) {
		label(tr::fresh_PluginOutdated(tr::now, lt_version, Utf8(info->testedWith)), st::pluginDangerMedium);
	}
	Ui::AddSkip(content);
	Ui::AddDivider(content);
	Ui::AddSkip(content);

	if (!broken) {
		const auto enabled = Settings::AddButtonWithIcon(
			content,
			tr::fresh_PluginEnabled(),
			st::settingsButton,
			{ &st::menuIconCustomize });
		enabled->toggleOn(rpl::single(info->enabled), true);
		enabled->addClickHandler([=, on = info->enabled] {
			Post([=](not_null<fgpl::PluginManager*> manager) {
				manager->setEnabled(plugin, !on);
			});
		});
		if (info->enabled && info->status != fgpl::PluginStatus::Running) {
			const auto weakBox = base::make_weak(box);
			Settings::AddButtonWithIcon(
				content,
				tr::fresh_PluginStart(),
				st::settingsButton,
				{ &st::menuIconRestore }
			)->addClickHandler([=] {
				Post([=](not_null<fgpl::PluginManager*> manager) {
					const auto started = manager->start(plugin);
					crl::on_main([=] {
						if (const auto strong = weakBox.get()) {
							strong->showToast(started
								? tr::fresh_PluginStarted(tr::now)
								: tr::fresh_PluginStartFailed(tr::now));
						}
					});
				});
			});
		}
	}
	if (parsed && parsed->settingsPage) {
		Settings::AddButtonWithIcon(
			content,
			tr::fresh_PluginSettingsButton(),
			st::settingsButton,
			{ &st::menuIconSettings }
		)->addClickHandler([=] {
			ShowPluginSettings(controller, Utf8(plugin));
		});
	}
	if (!broken || (parsed && parsed->settingsPage)) {
		Ui::AddSkip(content);
		Ui::AddDivider(content);
		Ui::AddSkip(content);
	}
	if (parsed) {
		const auto granted = snapshot->grantsOf(plugin);
		const auto total = int(parsed->permissions.size());
		const auto allowed = int(ranges::count_if(parsed->permissions, [&](const auto &request) {
			return request.required
				|| (granted && granted->granted.contains(request.id));
		}));
		Settings::AddButtonWithLabel(
			content,
			tr::fresh_PluginPermissions(),
			rpl::single(tr::fresh_PluginPermissionsCount(
				tr::now,
				lt_allowed,
				QString::number(allowed),
				lt_total,
				QString::number(total))),
			st::settingsButton,
			{ &st::menuIconPermissions }
		)->addClickHandler([=] {
			controller->show(Box(FillPermissionsBox, controller, plugin));
		});
	}
	Settings::AddButtonWithIcon(
		content,
		tr::fresh_PluginLog(),
		st::settingsButton,
		{ &st::menuIconInfo }
	)->addClickHandler([=] {
		controller->show(Box(FillLogBox, plugin, name));
	});
	if (UsesNetwork(*info)) {
		Settings::AddButtonWithIcon(
			content,
			tr::fresh_PluginsNetwork(),
			st::settingsButton,
			{ &st::menuIconNetwork }
		)->addClickHandler([=] {
			ShowNetworkPanel(controller);
		});
	}
	Settings::AddButtonWithIcon(
		content,
		tr::fresh_PluginUpdateFromFile(),
		st::settingsButton,
		{ &st::menuIconDownload }
	)->addClickHandler([=] {
		AyuUi::ChoosePluginFile(controller);
	});
	Ui::AddSkip(content);
	Ui::AddDividerText(
		content,
		rpl::single(AboutText(*snapshot, *info, parsed.get(), account)));
	Ui::AddSkip(content);

	const auto weak = base::make_weak(box);
	Settings::AddButtonWithIcon(
		content,
		tr::fresh_PluginRemove(),
		st::settingsAttentionButtonWithIcon,
		{ &st::menuIconDeleteAttention }
	)->addClickHandler([=] {
		controller->show(Ui::MakeConfirmBox({
			.text = tr::fresh_PluginsRemoveConfirmation(
				tr::now,
				lt_name,
				::Ui::Text::Bold(name),
				tr::marked),
			.confirmed = [=](Fn<void()> &&close) {
				Post([=](not_null<fgpl::PluginManager*> manager) {
					manager->uninstall(plugin, true);
				});
				ForgetSettingsPages(plugin);
				close();
				if (const auto strong = weak.get()) {
					strong->closeBox();
				}
			},
			.confirmText = tr::fresh_PluginsRemove(),
			.confirmStyle = &st::attentionBoxButton,
		}));
	});
}

[[nodiscard]] std::optional<QString> StateKey(const std::string &plugin) {
	const auto snapshot = Snapshot();
	const auto info = snapshot ? snapshot->plugin(plugin) : nullptr;
	if (!info) {
		return std::nullopt;
	}
	const auto granted = snapshot->grantsOf(plugin);
	auto permissions = QStringList();
	if (granted) {
		for (const auto &id : granted->granted) {
			permissions.push_back(Utf8(id));
		}
		permissions.push_back(granted->chats
			? QString::number(granted->chats->size())
			: u"all"_q);
	}
	return QStringList{
		Utf8(info->version),
		Utf8(info->problem),
		Utf8(info->disabledReason),
		QString::number(int(info->status)),
		QString::number(int(info->enabled)),
		QString::number(int(info->pendingUpdate)),
		QString::number(int(info->outdated)),
		QString::number(int(info->signedPackage)),
		Utf8(info->publisher),
		permissions.join(','),
	}.join('\n');
}

void FillDetailsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		std::string plugin) {
	box->setWidth(st::boxWideWidth);
	const auto content = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto shown = content->lifetime().make_state<QString>();
	const auto rebuild = [=] {
		const auto key = StateKey(plugin);
		if (key.has_value() && *key == *shown) {
			return;
		}
		*shown = key.value_or(QString());
		content->clear();
		BuildDetails(box, content, controller, plugin);
	};
	rebuild();
	ListChanges(
	) | rpl::on_next([=] {
		Ui::PostponeCall(content, rebuild);
	}, content->lifetime());
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

struct Dialogs {
	std::map<std::pair<std::string, std::string>, cbor::Value> pages;
	std::map<std::pair<std::string, std::string>, QPointer<Ui::GenericBox>> boxes;
	rpl::event_stream<std::pair<std::string, std::string>> changes;
};

[[nodiscard]] Dialogs &GetDialogs() {
	static auto result = Dialogs();
	return result;
}

[[nodiscard]] Main::Session *SessionFor(const std::string &account) {
	for (const auto &entry : Core::App().domain().accounts()) {
		if (const auto session = entry.account->maybeSession()) {
			if (AccountOf(session) == account) {
				return session;
			}
		}
	}
	return nullptr;
}

[[nodiscard]] QString PluginName(const std::string &id) {
	const auto snapshot = Snapshot();
	const auto parsed = snapshot ? snapshot->manifest(id) : nullptr;
	return parsed ? Utf8(parsed->name.get(Language())) : Utf8(id);
}

[[nodiscard]] QString AccountSuffix(not_null<Main::Session*> session) {
	return (Core::App().domain().accounts().size() > 1)
		? (u" · "_q + session->user()->name())
		: QString();
}

void FillDialogBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		std::string plugin,
		std::string account) {
	const auto key = std::make_pair(plugin, account);
	const auto session = SessionFor(account);
	const auto weak = session ? base::make_weak(session) : base::weak_ptr<Main::Session>();
	const auto suffix = session ? AccountSuffix(session) : QString();
	box->setWidth(st::boxWideWidth);
	AddSourceRow(box, PluginName(plugin), suffix);
	const auto content = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto closedByPlugin = box->lifetime().make_state<bool>(false);
	const auto shown = box->lifetime().make_state<std::shared_ptr<Page>>();
	const auto buttonsShown = box->lifetime().make_state<std::optional<QString>>();
	const auto rebuild = [=] {
		const auto &pages = GetDialogs().pages;
		const auto i = pages.find(key);
		if (i == pages.end() || !weak) {
			return;
		}
		const auto title = TextOf(i->second, "title");
		box->setTitle(rpl::single(title.isEmpty() ? PluginName(plugin) : title));
		ShowPage(content, controller, weak, plugin, &i->second, true, *shown);
		const auto signature = StructureOf(cbor::Value::FromMap({
			{ "buttons", i->second.find("buttons") ? *i->second.find("buttons") : cbor::Value() },
		}));
		if (*buttonsShown == signature) {
			return;
		}
		*buttonsShown = signature;
		box->clearButtons();
		const auto buttons = i->second.find("buttons");
		for (const auto &button : (buttons && buttons->asArray()) ? *buttons->asArray() : cbor::Array()) {
			const auto buttonId = TextOf(button, "id").toStdString();
			const auto style = TextOf(button, "style");
			box->addButton(
				rpl::single(TextOf(button, "label")),
				[=] {
					if (const auto strong = weak.get()) {
						SendSetting(strong, plugin, "dialog.click", buttonId, std::nullopt);
					}
				},
				(style == u"danger"_q) ? st::attentionBoxButton : st::defaultBoxButton);
		}
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	};
	rebuild();
	const auto closeByPlugin = [=] {
		*closedByPlugin = true;
		box->closeBox();
	};
	GetDialogs().changes.events(
	) | rpl::filter([=](const auto &changed) {
		return changed == key;
	}) | rpl::on_next([=] {
		if (!GetDialogs().pages.contains(key)) {
			closeByPlugin();
		} else {
			Ui::PostponeCall(content, rebuild);
		}
	}, content->lifetime());
	ListChanges(
	) | rpl::on_next([=] {
		const auto snapshot = Snapshot();
		const auto info = snapshot ? snapshot->plugin(plugin) : nullptr;
		if (!info || !info->enabled || info->status != fgpl::PluginStatus::Running) {
			GetDialogs().pages.erase(key);
			closeByPlugin();
		}
	}, content->lifetime());
	box->boxClosing() | rpl::on_next([=] {
		if (*shown) {
			(*shown)->flushAll();
		}
		auto &dialogs = GetDialogs();
		if (const auto i = dialogs.boxes.find(key);
			i != dialogs.boxes.end() && i->second.data() == box.get()) {
			dialogs.boxes.erase(i);
		}
		if (!*closedByPlugin) {
			dialogs.pages.erase(key);
			if (weak) {
				Post([=](not_null<fgpl::PluginManager*> manager) {
					manager->send(plugin, account, cbor::Value::FromMap({
						{ "type", cbor::Value::FromText("dialog.close") },
					}));
				}, false);
			}
		}
	}, box->lifetime());
	if (!buttonsShown->has_value()) {
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}
}

[[nodiscard]] QString Age(crl::time opened) {
	const auto seconds = std::max<crl::time>(0, (crl::now() - opened) / 1000);
	return (seconds < 60)
		? tr::fresh_PluginsNetworkSeconds(tr::now, lt_seconds, QString::number(seconds))
		: tr::fresh_PluginsNetworkMinutes(tr::now, lt_minutes, QString::number(seconds / 60));
}


[[nodiscard]] QString NetworkEventText(const NetworkEvent &event) {
	auto parts = QStringList{
		QDateTime::fromMSecsSinceEpoch(event.when).toString(u"HH:mm:ss"_q),
		PluginName(event.plugin.toStdString()),
		event.kind + ' ' + event.action + ' ' + event.target,
	};
	if (!event.result.isEmpty()) {
		parts.push_back(u"→ "_q + event.result);
	}
	if (event.action != u"open"_q) {
		parts.push_back(u"↓ "_q + Ui::FormatSizeText(qint64(event.received))
			+ u" ↑ "_q + Ui::FormatSizeText(qint64(event.sent)));
		parts.push_back(QString::number(event.duration) + u" ms"_q);
	}
	return parts.join(u"  "_q);
}

void FillNetworkLogBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(tr::fresh_PluginsNetworkLog());
	box->setWidth(st::boxWideWidth);
	const auto onlyErrors = box->lifetime().make_state<rpl::variable<bool>>(false);
	const auto filter = Settings::AddButtonWithIcon(
		box->verticalLayout(),
		tr::fresh_PluginsNetworkLogErrors(),
		st::settingsButtonNoIcon);
	filter->toggleOn(rpl::single(false));
	Ui::AddSkip(box->verticalLayout());
	const auto lines = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto view = box->lifetime().make_state<LogView>();
	const auto version = box->lifetime().make_state<std::optional<uint64>>();
	const auto show = [=](bool reset) {
		const auto now = NetworkLogVersion();
		if (!reset && *version == now) {
			return;
		}
		*version = now;
		const auto errors = onlyErrors->current();
		auto rows = std::vector<LogRow>();
		for (const auto &event : NetworkLog()) {
			if (errors && !event.failed) {
				continue;
			}
			const auto text = NetworkEventText(event);
			rows.push_back({
				.key = QString::number(event.when) + ' ' + text,
				.text = text,
				.st = event.failed ? &st::pluginDangerHigh : &st::boxLabel,
			});
		}
		ShowLogRows(box, lines, view, rows, reset, tr::fresh_PluginsNetworkLogEmpty(tr::now));
	};
	filter->toggledValue(
	) | rpl::on_next([=](bool value) {
		*onlyErrors = value;
		show(true);
	}, filter->lifetime());
	const auto timer = box->lifetime().make_state<base::Timer>([=] { show(false); });
	timer->callEach(1000);
	box->addLeftButton(tr::fresh_PluginLogClear(), [=] {
		ClearNetworkLog();
		show(true);
	});
	box->addButton(tr::fresh_PluginLogCopy(), [=] {
		auto text = QStringList();
		for (const auto &event : NetworkLog()) {
			if (!onlyErrors->current() || event.failed) {
				text.push_back(NetworkEventText(event));
			}
		}
		QGuiApplication::clipboard()->setText(text.join('\n'));
		box->showToast(tr::lng_text_copied(tr::now));
	});
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

[[nodiscard]] QString NetworkTotalText(const fgpl::PluginInfo &info) {
	return tr::fresh_PluginsNetworkTotal(
		tr::now,
		lt_received,
		Ui::FormatSizeText(qint64(info.stats.netReceived)),
		lt_sent,
		Ui::FormatSizeText(qint64(info.stats.netSent)));
}

[[nodiscard]] QString ConnectionText(const ConnectionView &view) {
	const auto endpoint = view.host + ':' + QString::number(view.port)
		+ ((view.address.isEmpty() || view.address == view.host)
			? QString()
			: u" ("_q + view.address + ')');
	return tr::fresh_PluginsNetworkRow(
		tr::now,
		lt_kind,
		view.kind,
		lt_endpoint,
		endpoint,
		lt_received,
		Ui::FormatSizeText(qint64(view.received)),
		lt_sent,
		Ui::FormatSizeText(qint64(view.sent)),
		lt_age,
		Age(view.opened));
}

void FillNetworkBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	struct State {
		QString layout;
		bool built = false;
		std::map<QString, std::unique_ptr<rpl::variable<QString>>> totals;
		std::map<int64, std::unique_ptr<rpl::variable<QString>>> rows;
	};
	box->setTitle(tr::fresh_PluginsNetwork());
	box->setWidth(st::boxWideWidth);
	const auto content = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto state = box->lifetime().make_state<State>();
	const auto refresh = [=] {
		const auto snapshot = Snapshot();
		const auto plugins = snapshot ? snapshot->plugins : std::vector<fgpl::PluginInfo>();
		const auto connections = Connections();
		auto shownPlugins = std::vector<const fgpl::PluginInfo*>();
		auto layout = QStringList();
		for (const auto &info : plugins) {
			const auto id = Utf8(info.id);
			const auto own = ranges::any_of(connections, [&](const ConnectionView &view) {
				return view.plugin == id;
			});
			if (!UsesNetwork(info) && !own) {
				continue;
			}
			shownPlugins.push_back(&info);
			layout.push_back(id);
			for (const auto &view : connections) {
				if (view.plugin == id) {
					layout.push_back(QString::number(view.id));
				}
			}
		}
		if (state->built && state->layout == layout.join('\n')) {
			for (const auto info : shownPlugins) {
				if (const auto i = state->totals.find(Utf8(info->id)); i != state->totals.end()) {
					*i->second = NetworkTotalText(*info);
				}
			}
			for (const auto &view : connections) {
				if (const auto i = state->rows.find(view.id); i != state->rows.end()) {
					*i->second = ConnectionText(view);
				}
			}
			return;
		}
		state->layout = layout.join('\n');
		state->built = true;
		content->clear();
		state->totals.clear();
		state->rows.clear();
		for (const auto info : shownPlugins) {
			const auto id = Utf8(info->id);
			Ui::AddSubsectionTitle(content, rpl::single(PluginName(info->id)));
			auto &total = state->totals[id];
			total = std::make_unique<rpl::variable<QString>>(NetworkTotalText(*info));
			content->add(object_ptr<Ui::FlatLabel>(
				content,
				total->value(),
				st::boxDividerLabel), st::boxRowPadding);
			auto own = 0;
			for (const auto &view : connections) {
				if (view.plugin != id) {
					continue;
				}
				++own;
				auto &text = state->rows[view.id];
				text = std::make_unique<rpl::variable<QString>>(ConnectionText(view));
				const auto connection = view.id;
				Settings::AddButtonWithLabel(
					content,
					text->value(),
					rpl::single(tr::fresh_PluginsNetworkClose(tr::now)),
					st::settingsButtonNoIcon
				)->addClickHandler([=] {
					CloseConnection(connection);
				});
			}
			if (!own) {
				content->add(object_ptr<Ui::FlatLabel>(
					content,
					tr::fresh_PluginsNetworkIdle(tr::now),
					st::boxDividerLabel), st::boxRowPadding);
			}
			Ui::AddSkip(content);
		}
		if (shownPlugins.empty()) {
			content->add(object_ptr<Ui::FlatLabel>(
				content,
				tr::fresh_PluginsNetworkEmpty(tr::now),
				st::boxLabel), st::boxRowPadding);
		}
	};
	refresh();
	const auto timer = box->lifetime().make_state<base::Timer>(refresh);
	timer->callEach(1000);
	box->addLeftButton(tr::fresh_PluginsNetworkLog(), [=] {
		controller->show(Box(FillNetworkLogBox));
	});
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace

namespace {

[[nodiscard]] QString RiskText(fgpl::Risk risk) {
	switch (risk) {
	case fgpl::Risk::ExfiltrationPossible: return tr::fresh_RiskExfiltration(tr::now);
	case fgpl::Risk::AnyServer: return tr::fresh_RiskAnyServer(tr::now);
	case fgpl::Risk::RawSockets: return tr::fresh_RiskSockets(tr::now);
	case fgpl::Risk::DeletedMessages: return tr::fresh_RiskDeleted(tr::now);
	case fgpl::Risk::SendsInYourName: return tr::fresh_RiskSends(tr::now);
	case fgpl::Risk::ChangesOutgoing: return tr::fresh_RiskOutgoing(tr::now);
	case fgpl::Risk::ChangesMessages: return tr::fresh_RiskChangesMessages(tr::now);
	case fgpl::Risk::Background: return tr::fresh_RiskBackground(tr::now);
	case fgpl::Risk::Clipboard: return tr::fresh_RiskClipboard(tr::now);
	case fgpl::Risk::Files: return tr::fresh_RiskFiles(tr::now);
	case fgpl::Risk::PluginMessages: return tr::fresh_RiskPlugins(tr::now);
	}
	return QString();
}

[[nodiscard]] const style::icon *PermissionIcon(const std::string &id) {
	static const auto icons = std::map<std::string, const style::icon*>{
		{ "messages.read", &st::menuIconChatBubble },
		{ "messages.read_deleted", &st::menuIconChatBubble },
		{ "messages.send", &st::menuIconChatBubble },
		{ "messages.transform_outgoing", &st::menuIconEdit },
		{ "messages.history", &st::menuIconChatBubble },
		{ "media.read", &st::menuIconFile },
		{ "chats.list", &st::menuIconChats },
		{ "chat.info", &st::menuIconInfo },
		{ "account.info", &st::menuIconProfile },
		{ "stars.balance", &st::menuIconEarn },
		{ "composer.draft", &st::menuIconEdit },
		{ "contacts.list", &st::menuIconProfile },
		{ "settings.read", &st::menuIconSettings },
		{ "folders.list", &st::menuIconShowInFolder },
		{ "ui.menu", &st::menuIconCustomize },
		{ "ui.command", &st::menuIconBotCommands },
		{ "ui.settings_page", &st::menuIconSettings },
		{ "ui.composer_button", &st::menuIconCustomize },
		{ "ui.dialog", &st::menuIconCustomize },
		{ "ui.toast", &st::menuIconInfo },
		{ "notifications", &st::menuIconNotifications },
		{ "net.https", &st::menuIconNetwork },
		{ "net.tcp", &st::menuIconNetwork },
		{ "net.udp", &st::menuIconNetwork },
		{ "storage.private", &st::menuIconDownload },
		{ "plugins.message", &st::menuIconBot },
		{ "fs.user_selected", &st::menuIconFile },
		{ "clipboard", &st::menuIconCopy },
		{ "background", &st::menuIconPowerUsage },
		{ "messages.edit", &st::menuIconEdit },
		{ "messages.delete", &st::menuIconDelete },
		{ "messages.react", &st::menuIconReactions },
		{ "users.info", &st::menuIconProfile },
		{ "chats.active", &st::menuIconShowInChat },
	};
	const auto i = icons.find(id);
	return (i != icons.end()) ? i->second : &st::menuIconLock;
}

[[nodiscard]] bool SeesChats(const fgpl::Manifest &manifest) {
	static const auto scoped = std::set<std::string>{
		"messages.read", "messages.read_deleted", "messages.history",
		"messages.send", "messages.transform_outgoing", "chats.list",
		"chat.info", "media.read", "composer.draft", "folders.list",
		"messages.edit", "messages.delete", "messages.react", "chats.active",
	};
	return ranges::any_of(manifest.permissions, [](const auto &request) {
		return scoped.contains(request.id);
	});
}

void ChooseChats(
		not_null<Window::SessionController*> controller,
		std::set<int64_t> current,
		Fn<void(std::set<int64_t>)> done) {
	const auto session = &controller->session();
	auto peers = base::flat_set<not_null<History*>>();
	auto hidden = std::set<int64_t>();
	for (const auto id : current) {
		if (const auto history = session->data().historyLoaded(PeerId(uint64(id)))) {
			peers.emplace(history);
		} else {
			hidden.insert(id);
		}
	}
	auto list = std::make_unique<EditFilterChatsListController>(
		session,
		tr::fresh_PluginChatsChoose(),
		Data::ChatFilter::Flags(),
		Data::ChatFilter::Flags(),
		peers,
		kMaxPluginChats,
		nullptr);
	auto initBox = [=](not_null<PeerListBox*> box) {
		box->addButton(tr::lng_settings_save(), crl::guard(box, [=] {
			auto chosen = hidden;
			for (const auto &peer : box->collectSelectedRows()) {
				chosen.insert(int64_t(peer->id.value));
			}
			done(std::move(chosen));
			box->closeBox();
		}));
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	};
	controller->show(Box<PeerListBox>(std::move(list), std::move(initBox)));
}

} // namespace

Fn<std::optional<fgpl::GrantDecision>()> AddPermissionsEditor(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		const fgpl::Manifest &manifest,
		const std::optional<fgpl::GrantDecision> &current,
		const std::vector<std::string> &added) {
	const auto container = box->verticalLayout();
	const auto granted = box->lifetime().make_state<
		rpl::variable<std::set<std::string>>>();
	auto initial = std::set<std::string>();
	auto checks = std::make_shared<std::vector<std::pair<std::string, DetailedSettingsButton*>>>();

	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		tr::fresh_PluginPermissionsAllAccounts(tr::now),
		st::boxDividerLabel));
	if (manifest.permissions.empty()) {
		Ui::AddSkip(container);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			tr::fresh_PluginNoPermissions(tr::now),
			st::boxLabel));
	}
	const auto refresh = [=] {
		auto now = std::set<std::string>();
		for (const auto &[id, button] : *checks) {
			if (button->toggled()) {
				now.insert(id);
			}
		}
		*granted = std::move(now);
	};
	for (const auto danger : { fgpl::Danger::High, fgpl::Danger::Medium, fgpl::Danger::Low }) {
		auto group = std::vector<const fgpl::PermissionRequest*>();
		for (const auto &request : manifest.permissions) {
			if (fgpl::PermissionDanger(request.id, request.scope) == danger) {
				group.push_back(&request);
			}
		}
		if (group.empty()) {
			continue;
		}
		Ui::AddSkip(container);
		Ui::AddSubsectionTitle(
			container,
			(danger == fgpl::Danger::High)
				? tr::fresh_PluginDangerHigh()
				: (danger == fgpl::Danger::Medium)
				? tr::fresh_PluginDangerMedium()
				: tr::fresh_PluginDangerLow());
		const auto background = (danger == fgpl::Danger::High)
			? &st::settingsIconBg1
			: (danger == fgpl::Danger::Medium)
			? &st::settingsIconBg3
			: &st::settingsIconBg2;
		for (const auto request : group) {
			const auto fresh = ranges::find(added, request->id) != end(added);
			const auto checked = request->required
				|| !current
				|| fresh
				|| current->granted.contains(request->id);
			if (checked) {
				initial.insert(request->id);
			}
			auto about = QStringList();
			if (request->required) {
				about.push_back(tr::fresh_PluginPermissionRequiredShort(tr::now));
			} else if (fresh) {
				about.push_back(tr::fresh_PluginPermissionNewShort(tr::now));
			}
			const auto reason = Utf8(request->reason.get(Language()));
			about.push_back(reason.isEmpty() ? Utf8(request->id) : reason);
			const auto button = AddDetailedSettingsButton(
				container,
				rpl::single(PermissionTitle(request->id, request->scope)),
				rpl::single(about.join(u" · "_q)),
				{ .icon = PermissionIcon(request->id), .background = background },
				rpl::single(checked),
				st::detailedSettingsButtonStyle);
			if (request->required) {
				button->setToggleLocked(true);
			}
			button->finishAnimating();
			button->toggledChanges() | rpl::on_next(refresh, button->lifetime());
			checks->emplace_back(request->id, button.get());
		}
	}
	*granted = std::move(initial);

	const auto chatsMode = std::make_shared<Ui::RadiobuttonGroup>(
		(current && current->chats)
			? 1
			: ranges::any_of(manifest.permissions, [](const auto &request) {
				return request.scope == "selected";
			})
			? 1
			: 0);
	const auto chats = std::make_shared<std::set<int64_t>>(
		(current && current->chats) ? *current->chats : std::set<int64_t>());
	const auto chatsShown = SeesChats(manifest);
	if (chatsShown) {
		Ui::AddSkip(container);
		Ui::AddSubsectionTitle(container, tr::fresh_PluginChats());
		const auto all = box->addRow(object_ptr<Ui::Radiobutton>(
			box,
			chatsMode,
			0,
			tr::fresh_PluginChatsAll(tr::now),
			st::defaultBoxCheckbox));
		const auto onlySelected = ranges::any_of(manifest.permissions, [](const auto &request) {
			return request.scope == "selected";
		});
		all->setDisabled(onlySelected);
		Ui::AddSkip(container, st::defaultVerticalListSkip / 2);
		box->addRow(object_ptr<Ui::Radiobutton>(
			box,
			chatsMode,
			1,
			tr::fresh_PluginChatsSelected(tr::now),
			st::defaultBoxCheckbox));
		const auto picker = container->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				container,
				object_ptr<Ui::VerticalLayout>(container)));
		const auto count = box->lifetime().make_state<rpl::variable<int>>(
			int(chats->size()));
		Ui::AddSkip(picker->entity(), st::defaultVerticalListSkip / 2);
		Settings::AddButtonWithLabel(
			picker->entity(),
			tr::fresh_PluginChatsChoose(),
			count->value() | rpl::map([](int value) {
				return tr::fresh_PluginChatsCount(
					tr::now,
					lt_amount,
					QString::number(value));
			}),
			st::settingsButton,
			{ &st::menuIconChats }
		)->addClickHandler([=] {
			ChooseChats(controller, *chats, crl::guard(box, [=](std::set<int64_t> chosen) {
				*chats = std::move(chosen);
				*count = int(chats->size());
			}));
		});
		picker->toggleOn(chatsMode->value() | rpl::map([](int value) {
			return value == 1;
		}));
		picker->finishAnimating();
		Ui::AddSkip(container);
		Ui::AddDividerText(container, tr::fresh_PluginChatsAbout());
	}

	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		granted->value() | rpl::map([](const std::set<std::string> &now) {
			auto lines = QStringList();
			for (const auto &note : fgpl::RiskNotes(now)) {
				lines.push_back(u"• "_q + RiskText(note.risk));
			}
			return lines.isEmpty()
				? tr::fresh_PluginRisksNone(tr::now)
				: (tr::fresh_PluginRisks(tr::now) + '\n' + lines.join('\n'));
		}));

	return [=]() -> std::optional<fgpl::GrantDecision> {
		auto result = fgpl::GrantDecision();
		for (const auto &[id, button] : *checks) {
			if (button->toggled()) {
				result.granted.insert(id);
			}
		}
		if (chatsShown && chatsMode->current() == 1) {
			if (chats->empty()) {
				controller->showToast(tr::fresh_PluginChatsNone(tr::now));
				return std::nullopt;
			}
			result.chats = *chats;
		}
		return result;
	};
}

PluginHeader HeaderFor(
		const fgpl::Manifest &manifest,
		const std::string &publisher,
		bool signedPackage,
		QByteArray icon) {
	return {
		.name = Utf8(manifest.name.get(Language())),
		.version = Utf8(manifest.version.text()),
		.author = Utf8(manifest.authorName),
		.description = Utf8(manifest.description.get(Language())),
		.license = Utf8(manifest.license),
		.homepage = Utf8(manifest.homepage.empty() ? manifest.authorUrl : manifest.homepage),
		.repository = Utf8(manifest.repository),
		.support = Utf8(manifest.support),
		.publisher = Utf8(publisher),
		.signedPackage = signedPackage,
		.icon = std::move(icon),
	};
}

void AddPluginHeader(
		not_null<Ui::VerticalLayout*> container,
		const PluginHeader &data) {
	const auto row = container->add(
		object_ptr<Ui::RpWidget>(container),
		st::boxRowPadding);
	const auto size = st::pluginHeaderIconSize;
	auto image = data.icon.isEmpty() ? QImage() : QImage::fromData(data.icon);
	if (!image.isNull()) {
		const auto ratio = style::DevicePixelRatio();
		image = image.scaled(
			size * ratio,
			size * ratio,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
		image.setDevicePixelRatio(ratio);
	}
	const auto letter = data.name.isEmpty() ? QString() : data.name.left(1).toUpper();
	row->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(row);
		PainterHighQualityEnabler hq(p);
		const auto rect = QRect(0, 0, size, size);
		auto path = QPainterPath();
		path.addRoundedRect(rect, st::pluginHeaderIconRadius, st::pluginHeaderIconRadius);
		if (!image.isNull()) {
			p.setClipPath(path);
			const auto shown = image.size() / image.devicePixelRatio();
			p.drawImage(
				QRect(
					(size - shown.width()) / 2,
					(size - shown.height()) / 2,
					shown.width(),
					shown.height()),
				image);
			return;
		}
		p.fillPath(path, st::settingsIconBg5);
		p.setPen(st::settingsIconFg);
		p.setFont(st::pluginHeaderLetterFont);
		p.drawText(rect, letter, style::al_center);
	}, row->lifetime());

	const auto title = Ui::CreateChild<Ui::FlatLabel>(
		row,
		data.name,
		st::pluginHeaderTitle);
	auto details = QStringList{ tr::fresh_PluginVersionShort(tr::now, lt_version, data.version) };
	if (!data.author.isEmpty()) {
		details.push_back(tr::fresh_PluginBy(tr::now, lt_author, data.author));
	}
	const auto subtitle = Ui::CreateChild<Ui::FlatLabel>(
		row,
		details.join(u" · "_q),
		st::pluginHeaderSubtitle);
	const auto badge = Ui::CreateChild<Ui::FlatLabel>(
		row,
		data.signedPackage
			? (u"✓ "_q + tr::fresh_PluginSignedBy(tr::now, lt_publisher, data.publisher))
			: (u"⚠ "_q + tr::fresh_PluginNotSigned(tr::now)),
		data.signedPackage ? st::pluginHeaderSigned : st::pluginHeaderUnsigned);
	for (const auto label : { title, subtitle, badge }) {
		label->show();
	}
	row->widthValue() | rpl::on_next([=](int width) {
		const auto left = size + st::pluginHeaderSkip;
		const auto available = std::max(width - left, 1);
		auto top = 0;
		for (const auto label : { title, subtitle, badge }) {
			label->resizeToWidth(available);
			label->moveToLeft(left, top, width);
			top += label->height() + st::pluginHeaderLineSkip;
		}
		const auto height = std::max(size, top);
		if (row->height() != height) {
			row->resize(width, height);
		}
	}, row->lifetime());

	if (!data.description.isEmpty()) {
		Ui::AddSkip(container);
		container->add(
			object_ptr<Ui::FlatLabel>(container, data.description, st::boxLabel),
			st::boxRowPadding);
	}
	auto links = TextWithEntities();
	const auto link = [&](const QString &url, const QString &text) {
		if (url.isEmpty()) {
			return;
		} else if (!links.text.isEmpty()) {
			links.append(u" · "_q);
		}
		links.append(::Ui::Text::Link(text, url));
	};
	link(data.homepage, tr::fresh_PluginHomepage(tr::now));
	link(data.repository, tr::fresh_PluginRepository(tr::now));
	link(data.support, tr::fresh_PluginSupport(tr::now));
	if (!data.license.isEmpty()) {
		if (!links.text.isEmpty()) {
			links.append(u" · "_q);
		}
		links.append(tr::fresh_PluginLicense(tr::now, lt_license, data.license));
	}
	if (!links.text.isEmpty()) {
		Ui::AddSkip(container, st::defaultVerticalListSkip / 2);
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(links),
				st::boxDividerLabel),
			st::boxRowPadding);
	}
}

QString AcceptsFrom(const std::vector<std::string> &senders) {
	auto names = QStringList();
	for (const auto &sender : senders) {
		names.push_back((sender == "*")
			? tr::fresh_PluginAcceptsAny(tr::now)
			: QString::fromStdString(sender));
	}
	return tr::fresh_PluginAcceptsFrom(tr::now, lt_list, names.join(u", "_q));
}

QString PermissionTitle(const std::string &id, const std::string &scope) {
	if (id == "messages.read" && scope == "all") {
		return tr::fresh_PermMessagesReadAll(tr::now);
	} else if (id == "messages.read" && scope == "selected") {
		return tr::fresh_PermMessagesReadSelected(tr::now);
	}
	static const auto titles = std::map<std::string, tr::phrase<>>{
		{ "messages.read", tr::fresh_PermMessagesRead },
		{ "messages.read_deleted", tr::fresh_PermMessagesReadDeleted },
		{ "messages.send", tr::fresh_PermMessagesSend },
		{ "messages.transform_outgoing", tr::fresh_PermMessagesTransform },
		{ "media.read", tr::fresh_PermMediaRead },
		{ "chats.list", tr::fresh_PermChatsList },
		{ "chat.info", tr::fresh_PermChatInfo },
		{ "messages.history", tr::fresh_PermMessagesHistory },
		{ "account.info", tr::fresh_PermAccountInfo },
		{ "stars.balance", tr::fresh_PermStarsBalance },
		{ "composer.draft", tr::fresh_PermComposerDraft },
		{ "contacts.list", tr::fresh_PermContactsList },
		{ "settings.read", tr::fresh_PermSettingsRead },
		{ "folders.list", tr::fresh_PermFoldersList },
		{ "ui.menu", tr::fresh_PermUiMenu },
		{ "ui.command", tr::fresh_PermUiCommand },
		{ "ui.settings_page", tr::fresh_PermUiSettingsPage },
		{ "ui.composer_button", tr::fresh_PermUiComposerButton },
		{ "ui.dialog", tr::fresh_PermUiDialog },
		{ "ui.toast", tr::fresh_PermUiToast },
		{ "notifications", tr::fresh_PermNotifications },
		{ "net.https", tr::fresh_PermNetHttps },
		{ "net.tcp", tr::fresh_PermNetTcp },
		{ "net.udp", tr::fresh_PermNetUdp },
		{ "storage.private", tr::fresh_PermStorage },
		{ "plugins.message", tr::fresh_PermPluginsMessage },
		{ "fs.user_selected", tr::fresh_PermFiles },
		{ "clipboard", tr::fresh_PermClipboard },
		{ "background", tr::fresh_PermBackground },
		{ "messages.edit", tr::fresh_PermMessagesEdit },
		{ "messages.delete", tr::fresh_PermMessagesDelete },
		{ "messages.react", tr::fresh_PermMessagesReact },
		{ "users.info", tr::fresh_PermUsersInfo },
		{ "chats.active", tr::fresh_PermChatsActive },
	};
	const auto i = titles.find(id);
	return (i != titles.end()) ? i->second(tr::now) : Utf8(id);
}

QString PermissionLabel(const std::string &id) {
	return PermissionTitle(id, std::string());
}

bool HasComposerItems(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer) {
	return !EntriesFor(session, "composer", peer).empty();
}

class ComposerButton final
	: public Ui::IconButton
	, public Ui::AbstractTooltipShower {
public:
	ComposerButton(QWidget *parent, Entry entry, bool more)
	: IconButton(parent, st::historyPluginsToggle)
	, _entry(std::move(entry))
	, _more(more) {
		setAccessibleName(_more ? tr::fresh_Plugins(tr::now) : _entry.text);
	}

	[[nodiscard]] const Entry &entry() const {
		return _entry;
	}

	QString tooltipText() const override {
		return _more ? tr::fresh_Plugins(tr::now) : _entry.text;
	}
	QPoint tooltipPos() const override {
		return QCursor::pos();
	}
	bool tooltipWindowActive() const override {
		return Ui::AppInFocus() && Ui::InFocusChain(window());
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		const auto &st = this->st();
		paintRipple(p, st.rippleAreaPosition);
		const auto color = anim::color(
			st::historyComposeIconFg,
			st::historyComposeIconFgOver,
			iconOverOpacity());
		const auto size = st::menuIconBot.size();
		const auto position = QPoint(
			(width() - size.width()) / 2,
			(height() - size.height()) / 2);
		if (_more) {
			st::menuIconBot.paint(p, position, width(), color);
		} else {
			PaintPluginIcon(p, position, width(), _entry.plugin, _entry.icon, st::menuIconBot, color);
		}
	}
	void enterEventHook(QEnterEvent *e) override {
		Ui::Tooltip::Show(1000, this);
		IconButton::enterEventHook(e);
	}
	void leaveEventHook(QEvent *e) override {
		Ui::Tooltip::Hide();
		IconButton::leaveEventHook(e);
	}

private:
	const Entry _entry;
	const bool _more = false;

};

struct ComposerButtons::State {
	not_null<Window::SessionController*> controller;
	std::vector<base::unique_qptr<ComposerButton>> buttons;
	base::unique_qptr<Ui::PopupMenu> menu;
	std::vector<Entry> overflow;
	QString key;
	PeerId peer = 0;
	MsgId topicRootId = 0;
	Fn<MsgId()> replyTo;
	Fn<void()> replyTaken;
};

ComposerButtons::ComposerButtons(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: RpWidget(parent)
, _state(std::make_unique<State>(State{ .controller = controller })) {
	resize(0, st::historyPluginsToggle.height);
}

ComposerButtons::~ComposerButtons() = default;

bool ComposerButtons::empty() const {
	return _state->buttons.empty();
}

void ComposerButtons::setReply(Fn<MsgId()> current, Fn<void()> taken) {
	_state->replyTo = std::move(current);
	_state->replyTaken = std::move(taken);
}

bool ComposerButtons::refresh(PeerData *peer, MsgId topicRootId) {
	const auto session = &_state->controller->session();
	const auto entries = peer
		? EntriesFor(session, "composer", peer)
		: std::vector<Entry>();
	auto key = QStringList();
	for (const auto &entry : entries) {
		key.push_back(Utf8(entry.plugin + '/' + entry.menuId + '/' + entry.icon) + ' ' + entry.text);
	}
	_state->peer = peer ? peer->id : PeerId();
	_state->topicRootId = topicRootId;
	if (_state->key == key.join('\n')) {
		return false;
	}
	_state->key = key.join('\n');
	_state->buttons.clear();
	_state->overflow.clear();
	const auto weak = base::make_weak(session);
	const auto click = [=](const Entry &entry) {
		const auto strong = weak.get();
		const auto found = strong ? strong->data().peerLoaded(_state->peer) : nullptr;
		if (!found) {
			return;
		}
		const auto replyToId = _state->replyTo ? _state->replyTo() : MsgId();
		Send(strong, entry.plugin, ClickEvent(
			"composer",
			entry.menuId,
			found,
			nullptr,
			_state->topicRootId,
			replyToId));
		if (replyToId && _state->replyTaken) {
			// The plugin answers to it, like a sent message would.
			_state->replyTaken();
		}
	};
	const auto fits = (int(entries.size()) <= kMaxComposerButtons);
	const auto direct = fits ? int(entries.size()) : (kMaxComposerButtons - 1);
	for (auto i = 0; i != int(entries.size()); ++i) {
		if (i >= direct) {
			_state->overflow.push_back(entries[i]);
			continue;
		}
		auto button = base::make_unique_q<ComposerButton>(this, entries[i], false);
		const auto entry = entries[i];
		button->setClickedCallback([=] { click(entry); });
		_state->buttons.push_back(std::move(button));
	}
	if (!_state->overflow.empty()) {
		auto more = base::make_unique_q<ComposerButton>(this, Entry(), true);
		const auto raw = more.get();
		more->setClickedCallback([=] {
			_state->menu = base::make_unique_q<Ui::PopupMenu>(raw, st::popupMenuWithIcons);
			const auto add = Ui::Menu::CreateAddActionCallback(_state->menu.get());
			for (const auto &entry : _state->overflow) {
				AddPluginAction(add, entry, [=] { click(entry); });
			}
			_state->menu->popup(raw->mapToGlobal(QPoint(raw->width(), 0)));
		});
		_state->buttons.push_back(std::move(more));
	}
	auto left = 0;
	for (const auto &button : _state->buttons) {
		button->moveToLeft(left, 0);
		button->show();
		left += button->width();
	}
	resize(left, st::historyPluginsToggle.height);
	return true;
}

QString TrayItemsKey() {
	const auto session = Core::App().domain().started()
		? Core::App().activeAccount().maybeSession()
		: nullptr;
	auto key = QStringList();
	for (const auto &entry : session ? EntriesFor(session, "tray") : std::vector<Entry>()) {
		key.push_back(Utf8(entry.plugin + '/' + entry.menuId) + ' ' + entry.text);
	}
	return key.join('\n');
}

void AddTrayItems(Fn<void(const QString &text, Fn<void()> handler)> add) {
	const auto session = Core::App().domain().started()
		? Core::App().activeAccount().maybeSession()
		: nullptr;
	if (!session) {
		return;
	}
	const auto weak = base::make_weak(session);
	for (const auto &entry : EntriesFor(session, "tray")) {
		add(entry.text, [=] {
			if (const auto strong = weak.get()) {
				Send(strong, entry.plugin, ClickEvent("tray", entry.menuId, nullptr, nullptr));
			}
		});
	}
}

void ShowPluginDialog(
		const std::string &id,
		const std::string &account,
		const cbor::Value *page) {
	auto &dialogs = GetDialogs();
	const auto key = std::make_pair(id, account);
	if (!page) {
		dialogs.pages.erase(key);
		dialogs.changes.fire_copy(key);
		return;
	}
	const auto refuse = [&] {
		dialogs.pages.erase(key);
		Post([=](not_null<fgpl::PluginManager*> manager) {
			manager->send(id, account, cbor::Value::FromMap({
				{ "type", cbor::Value::FromText("dialog.close") },
			}));
		}, false);
	};
	const auto session = SessionFor(account);
	if (!session || Core::App().passcodeLocked()) {
		refuse();
		return;
	}
	if (const auto i = dialogs.boxes.find(key); i != dialogs.boxes.end() && i->second) {
		dialogs.pages[key] = *page;
		dialogs.changes.fire_copy(key);
		return;
	}
	dialogs.pages[key] = *page;
	const auto ours = [&](Window::Controller *window) {
		const auto controller = window ? window->sessionController() : nullptr;
		return (controller && &controller->session() == session) ? controller : nullptr;
	};
	const auto separate = Window::SeparateId(&session->account());
	auto controller = ours(Core::App().windowFor(separate));
	if (!controller) {
		controller = ours(Core::App().ensureSeparateWindowFor(separate));
	}
	if (!controller) {
		refuse();
		return;
	}
	auto box = Box(FillDialogBox, controller, id, account);
	dialogs.boxes[key] = box.data();
	controller->show(std::move(box));
}

void ShowNetworkLog(not_null<Window::SessionController*> controller) {
	controller->show(Box(FillNetworkLogBox));
}

void ShowNetworkPanel(not_null<Window::SessionController*> controller) {
	controller->show(Box(FillNetworkBox, controller));
}

void ForgetSettingsPages(const std::string &id) {
	auto &pages = GetPages();
	auto removed = std::vector<std::pair<std::string, std::string>>();
	for (auto i = pages.pages.begin(); i != pages.pages.end();) {
		if (i->first.first == id) {
			removed.push_back(i->first);
			i = pages.pages.erase(i);
		} else {
			++i;
		}
	}
	for (const auto &key : removed) {
		pages.changes.fire_copy(key);
	}
}

void ShowPluginDetails(
		not_null<Window::SessionController*> controller,
		const QString &pluginId) {
	controller->show(Box(FillDetailsBox, controller, pluginId.toStdString()));
}

void StoreSettingsPage(
		const std::string &id,
		const std::string &account,
		const cbor::Value &page) {
	auto &pages = GetPages();
	const auto key = std::make_pair(id, account);
	pages.pages[key] = page;
	pages.changes.fire_copy(key);
}

void AddMessageMenuItems(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item) {
	const auto peer = item->history()->peer;
	if (item->isService() || !item->isRegular() || !IsServerMsgId(item->id)) {
		return;
	}
	const auto session = &item->history()->session();
	const auto entries = EntriesFor(session, "message", peer);
	if (entries.empty()) {
		return;
	}
	menu->addSeparator();
	const auto itemId = item->fullId();
	const auto weak = base::make_weak(session);
	const auto add = Ui::Menu::CreateAddActionCallback(menu);
	for (const auto &entry : entries) {
		AddPluginAction(add, entry, [=] {
			const auto strong = weak.get();
			const auto found = strong ? strong->data().message(itemId) : nullptr;
			if (!found) {
				return;
			}
			Send(strong, entry.plugin, ClickEvent(
				"message",
				entry.menuId,
				found->history()->peer,
				found));
		});
	}
}

void AddPeerMenuItems(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId topicRootId,
		bool profile,
		const Ui::Menu::MenuCallback &add) {
	if (FreshSecret::IsSecretPeerId(peer->id)) {
		return;
	}
	const auto surface = profile ? "profile" : "chat";
	const auto session = &controller->session();
	const auto weak = base::make_weak(session);
	const auto peerId = peer->id;
	const auto entries = EntriesFor(session, surface, peer);
	if (!entries.empty()) {
		add({ .isSeparator = true });
	}
	for (const auto &entry : entries) {
		AddPluginAction(add, entry, [=] {
			const auto strong = weak.get();
			const auto found = strong ? strong->data().peerLoaded(peerId) : nullptr;
			if (found) {
				Send(strong, entry.plugin, ClickEvent(surface, entry.menuId, found, nullptr, topicRootId));
			}
		});
	}
}

void AddMainMenuItems(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	const auto inner = container->add(object_ptr<Ui::VerticalLayout>(container));
	const auto session = &controller->session();
	const auto weak = base::make_weak(session);
	const auto shown = inner->lifetime().make_state<std::optional<QString>>();
	const auto rebuild = [=] {
		const auto strong = weak.get();
		const auto entries = strong
			? EntriesFor(strong, "main_menu")
			: std::vector<Entry>();
		auto key = QStringList();
		for (const auto &entry : entries) {
			key.push_back(Utf8(entry.plugin + '/' + entry.menuId + '/' + entry.icon) + ' ' + entry.text);
		}
		if (*shown == key.join('\n')) {
			return;
		}
		*shown = key.join('\n');
		inner->clear();
		for (const auto &entry : entries) {
			const auto builtin = BuiltinIcon(entry.icon);
			const auto custom = !builtin && HasCustomIcon(entry.plugin, entry.icon);
			const auto button = FA::Ui::AddMainMenuButton(
				inner,
				rpl::single(entry.text),
				st::mainMenuButton,
				{ custom ? nullptr : builtin ? builtin : &st::menuIconBot });
			if (custom) {
				AddButtonIcon(
					button,
					st::mainMenuButton,
					entry.plugin,
					entry.icon,
					FA::Ui::Enabled() ? std::make_optional(kNavDrawerIconLeft) : std::nullopt);
			}
			button->setClickedCallback([=] {
				if (const auto again = weak.get()) {
					Send(again, entry.plugin, ClickEvent("main_menu", entry.menuId, nullptr, nullptr));
				}
			});
		}
		if (container->width() > 0) {
			inner->resizeToWidth(container->width());
		}
	};
	rebuild();
	ListChanges(
	) | rpl::on_next([=] {
		Ui::PostponeCall(inner, rebuild);
	}, inner->lifetime());
}

[[nodiscard]] bool BotHasCommand(not_null<PeerData*> peer, const QString &name) {
	const auto matches = [&](const std::vector<Data::BotCommand> &commands) {
		return ranges::any_of(commands, [&](const Data::BotCommand &command) {
			return !command.command.compare(name, Qt::CaseInsensitive);
		});
	};
	if (const auto user = peer->asUser()) {
		return user->isBot() && user->botInfo && matches(user->botInfo->commands);
	}
	const auto list = peer->isChat()
		? &peer->asChat()->botCommands()
		: peer->isMegagroup()
		? &peer->asChannel()->mgInfo->botCommands()
		: nullptr;
	return list && ranges::any_of(*list, [&](const auto &entry) {
		return matches(entry.second);
	});
}

std::vector<PluginCommand> CommandsFor(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer) {
	auto result = std::vector<PluginCommand>();
	const auto snapshot = Snapshot();
	if (!snapshot
		|| FreshSecret::IsSecretPeerId(peer->id)
		|| int64_t(peer->id.value) == fgpl::api::kServiceChat) {
		return result;
	}
	for (const auto &plugin : snapshot->contributions(AccountOf(session), Language())) {
		const auto grant = snapshot->grantsOf(plugin.id);
		const auto info = snapshot->plugin(plugin.id);
		const auto parsed = snapshot->manifest(plugin.id);
		const auto startable = info
			&& (info->status == fgpl::PluginStatus::Running
				|| (parsed && parsed->activation.contains("command")));
		if (!startable
			|| (grant
				&& grant->chats
				&& !grant->chats->contains(int64_t(peer->id.value)))) {
			continue;
		}
		for (const auto &command : plugin.commands) {
			result.push_back({
				.name = Utf8(command.name),
				.description = Utf8(command.description).isEmpty()
					? Utf8(plugin.name)
					: (Utf8(command.description) + u" · "_q + Utf8(plugin.name)),
				.plugin = Utf8(plugin.id),
			});
		}
	}
	for (auto &command : result) {
		command.ambiguous = BotHasCommand(peer, command.name)
			|| (ranges::count(result, command.name, &PluginCommand::name) > 1);
	}
	return result;
}

CommandResult RunCommand(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId topicRootId,
		MsgId replyToId,
		const QString &text) {
	const auto trimmed = text.trimmed();
	const auto snapshot = Snapshot();
	if (!snapshot
		|| !trimmed.startsWith('/')
		|| FreshSecret::IsSecretPeerId(peer->id)) {
		return CommandResult::None;
	}
	const auto space = trimmed.indexOf(QRegularExpression(u"\\s"_q));
	const auto head = trimmed.mid(1, space < 0 ? -1 : space - 1);
	const auto at = head.indexOf('@');
	const auto name = (at < 0 ? head : head.mid(0, at)).toLower();
	const auto target = (at < 0) ? QString() : head.mid(at + 1).toLower();
	if (name.isEmpty()) {
		return CommandResult::None;
	}
	const auto args = (space < 0) ? QString() : trimmed.mid(space + 1).trimmed();
	const auto session = &controller->session();
	for (const auto &plugin : snapshot->contributions(AccountOf(session), Language())) {
		if (!target.isEmpty() && Utf8(plugin.id).toLower() != target) {
			continue;
		}
		const auto grant = snapshot->grantsOf(plugin.id);
		if (grant
			&& grant->chats
			&& !grant->chats->contains(int64_t(peer->id.value))) {
			continue;
		}
		for (const auto &command : plugin.commands) {
			if (Utf8(command.name) != name) {
				continue;
			} else if (target.isEmpty() && BotHasCommand(peer, name)) {
				return CommandResult::None;
			}
			const auto info = snapshot->plugin(plugin.id);
			const auto parsed = snapshot->manifest(plugin.id);
			const auto startable = parsed && parsed->activation.contains("command");
			if (!info
				|| (info->status != fgpl::PluginStatus::Running && !startable)) {
				controller->showToast(tr::fresh_PluginNotRunning(tr::now));
				return CommandResult::Refused;
			}
			auto map = cbor::Map{
				{ "type", cbor::Value::FromText("command") },
				{ "name", Text(name) },
				{ "args", Text(args) },
				{ "chat", Number(int64_t(peer->id.value)) },
				{ "secret", Flag(false) },
			};
			if (topicRootId) {
				map.push_back({ "topic", Number(topicRootId.bare) });
			}
			if (replyToId) {
				map.push_back({ "reply_to", Number(replyToId.bare) });
			}
			Send(session, plugin.id, cbor::Value::FromMap(std::move(map)));
			return CommandResult::Sent;
		}
	}
	return CommandResult::None;
}

void ShowPluginSettings(
		not_null<Window::SessionController*> controller,
		const QString &pluginId) {
	const auto snapshot = Snapshot();
	const auto id = pluginId.toStdString();
	const auto account = AccountOf(&controller->session());
	const auto info = snapshot ? snapshot->plugin(id) : nullptr;
	if (!info || !info->enabled) {
		controller->showToast(tr::fresh_PluginNotRunning(tr::now));
		return;
	}
	Send(&controller->session(), id, cbor::Value::FromMap({
		{ "type", cbor::Value::FromText("settings.open") },
	}));
	const auto parsed = snapshot->manifest(id);
	const auto name = parsed ? Utf8(parsed->name.get(Language())) : pluginId;
	controller->show(Box(FillSettingsBox, controller, id, account, name));
}

#else

void StoreSettingsPage(
	const std::string &,
	const std::string &,
	const fgpl::cbor::Value &) {
}

void AddMessageMenuItems(not_null<Ui::PopupMenu*>, not_null<HistoryItem*>) {
}

void AddPeerMenuItems(
	not_null<Window::SessionController*>,
	not_null<PeerData*>,
	MsgId,
	bool,
	const Ui::Menu::MenuCallback &) {
}

void AddMainMenuItems(
	not_null<Ui::VerticalLayout*>,
	not_null<Window::SessionController*>) {
}

CommandResult RunCommand(
		not_null<Window::SessionController*>,
		not_null<PeerData*>,
		MsgId,
		MsgId,
		const QString &) {
	return CommandResult::None;
}

std::vector<PluginCommand> CommandsFor(
		not_null<Main::Session*>,
		not_null<PeerData*>) {
	return {};
}

void ShowPluginSettings(not_null<Window::SessionController*>, const QString &) {
}

void ShowPluginDetails(not_null<Window::SessionController*>, const QString &) {
}

QString PermissionTitle(const std::string &id, const std::string &) {
	return QString::fromStdString(id);
}

QString PermissionLabel(const std::string &id) {
	return QString::fromStdString(id);
}

QString AcceptsFrom(const std::vector<std::string> &) {
	return QString();
}

bool HasComposerItems(not_null<Main::Session*>, not_null<PeerData*>) {
	return false;
}

struct ComposerButtons::State {
};

ComposerButtons::ComposerButtons(
	QWidget *parent,
	not_null<Window::SessionController*>)
: RpWidget(parent)
, _state(std::make_unique<State>()) {
}

ComposerButtons::~ComposerButtons() = default;

bool ComposerButtons::refresh(PeerData *, MsgId) {
	return false;
}

bool ComposerButtons::empty() const {
	return true;
}

void ComposerButtons::setReply(Fn<MsgId()>, Fn<void()>) {
}

void AddTrayItems(Fn<void(const QString &text, Fn<void()> handler)>) {
}

QString TrayItemsKey() {
	return QString();
}

void ShowNetworkLog(not_null<Window::SessionController*>) {
}

void ShowNetworkPanel(not_null<Window::SessionController*>) {
}

void ForgetSettingsPages(const std::string &) {
}

#endif

} // namespace FreshPlugins
