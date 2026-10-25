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
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/manager.h"
#include "fgpl/permissions.h"
#endif

#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include <map>

namespace FreshPlugins {

#ifdef FRESHGRAM_PLUGINS

namespace {

namespace cbor = fgpl::cbor;

constexpr auto kMaxPluginChats = 100;
constexpr auto kSettingsWaitMs = crl::time(5000);

struct Entry {
	std::string plugin;
	std::string menuId;
	QString text;
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
		MsgId topicRootId = MsgId()) {
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

[[nodiscard]] QString StructureOf(const cbor::Value &page) {
	const auto items = page.find("items");
	auto result = QStringList();
	for (const auto &item : (items && items->asArray()) ? *items->asArray() : cbor::Array()) {
		result.push_back(TextOf(item, "type") + '/' + TextOf(item, "id"));
	}
	return result.join('\n');
}

[[nodiscard]] std::shared_ptr<Page> BuildSettingsContent(
		not_null<Ui::VerticalLayout*> content,
		not_null<Window::SessionController*> controller,
		base::weak_ptr<Main::Session> weak,
		const std::string &plugin,
		const cbor::Value &page,
		bool dialog) {
	auto result = std::make_shared<Page>();
	result->structure = StructureOf(page);
	const auto raw = result.get();
	const auto change = dialog ? "dialog.change" : "settings.change";
	const auto click = dialog ? "dialog.click" : "settings.click";
	const auto items = page.find("items");
	for (const auto &item : (items && items->asArray()) ? *items->asArray() : cbor::Array()) {
		const auto type = TextOf(item, "type");
		const auto itemId = TextOf(item, "id").toStdString();
		const auto label = content->lifetime().make_state<rpl::variable<QString>>(
			TextOf(item, "label"));
		auto update = Fn<void(const cbor::Value &)>();
		if (type == u"text"_q) {
			const auto text = content->add(
				object_ptr<Ui::FlatLabel>(content, TextOf(item, "text"), st::boxLabel),
				st::boxRowPadding);
			Ui::AddSkip(content);
			update = [=](const cbor::Value &now) {
				text->setText(TextOf(now, "text"));
			};
		} else if (type == u"divider"_q) {
			Ui::AddDivider(content);
		} else if (type == u"toggle"_q) {
			const auto button = Settings::AddButtonWithIcon(
				content,
				label->value(),
				st::settingsButtonNoIcon);
			const auto on = button->lifetime().make_state<rpl::variable<bool>>(
				item.find("value") && item.find("value")->asBool().value_or(false));
			button->toggleOn(on->value(), true);
			button->addClickHandler([=] {
				*on = !on->current();
				if (const auto strong = weak.get()) {
					SendSetting(strong, plugin, change, itemId, Flag(on->current()));
				}
			});
			update = crl::guard(button, [=](const cbor::Value &now) {
				*label = TextOf(now, "label");
				*on = now.find("value") && now.find("value")->asBool().value_or(false);
			});
		} else if (type == u"button"_q) {
			const auto button = Settings::AddButtonWithIcon(
				content,
				label->value(),
				st::settingsButtonNoIcon);
			button->addClickHandler([=] {
				if (const auto strong = weak.get()) {
					SendSetting(strong, plugin, click, itemId, std::nullopt);
				}
			});
			update = [=](const cbor::Value &now) {
				*label = TextOf(now, "label");
			};
		} else if (type == u"input"_q) {
			const auto field = content->add(
				object_ptr<Ui::InputField>(
					content,
					st::defaultInputField,
					label->value(),
					TextOf(item, "value")),
				st::boxRowPadding);
			const auto pending = field->lifetime().make_state<bool>(false);
			const auto sendNow = crl::guard(field, [=] {
				if (!*pending) {
					return;
				}
				*pending = false;
				if (const auto strong = weak.get()) {
					SendSetting(
						strong,
						plugin,
						change,
						itemId,
						Text(field->getLastText()));
				}
			});
			const auto timer = field->lifetime().make_state<base::Timer>(sendNow);
			field->changes() | rpl::on_next([=] {
				*pending = true;
				timer->callOnce(600);
			}, field->lifetime());
			field->focusedChanges(
			) | rpl::filter(!rpl::mappers::_1) | rpl::on_next([=] {
				sendNow();
			}, field->lifetime());
			raw->flush.push_back(sendNow);
			Ui::AddSkip(content);
			update = crl::guard(field, [=](const cbor::Value &now) {
				*label = TextOf(now, "label");
				const auto value = TextOf(now, "value");
				if (!field->hasFocus() && !*pending && field->getLastText() != value) {
					field->setText(value);
				}
			});
		} else if (type == u"select"_q) {
			struct Choice {
				std::vector<std::string> values;
				std::vector<QString> labels;
				int selected = 0;
			};
			const auto choice = std::make_shared<Choice>();
			const auto current = content->lifetime().make_state<rpl::variable<QString>>();
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
			const auto button = Settings::AddButtonWithLabel(
				content,
				label->value(),
				current->value(),
				st::settingsButtonNoIcon);
			button->addClickHandler([=] {
				controller->show(Box(SingleChoiceBox, SingleChoiceBoxArgs{
					.title = label->value(),
					.options = choice->labels,
					.initialSelection = choice->selected,
					.callback = crl::guard(button, [=](int index) {
						if (index < 0 || index >= int(choice->values.size())) {
							return;
						}
						choice->selected = index;
						*current = choice->labels[index];
						if (const auto strong = weak.get()) {
							SendSetting(
								strong,
								plugin,
								change,
								itemId,
								cbor::Value::FromText(choice->values[index]));
						}
					}),
				}));
			});
			update = crl::guard(button, [=](const cbor::Value &now) {
				*label = TextOf(now, "label");
				read(now);
			});
		}
		result->update.push_back(std::move(update));
	}
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
	} else if (shown && shown->structure == StructureOf(*page)) {
		shown->apply(*page);
		return;
	}
	if (shown) {
		shown->flushAll();
	}
	content->clear();
	shown = BuildSettingsContent(content, controller, weak, plugin, *page, dialog);
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
		return tr::fresh_PluginBadge(tr::now, lt_title, text, lt_plugin, pluginName);
	}));
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
	const auto shown = box->lifetime().make_state<std::optional<QString>>();
	const auto rebuild = [=] {
		const auto snapshot = Snapshot();
		const auto all = snapshot
			? snapshot->logLines(plugin)
			: std::vector<fgpl::LogLine>();
		const auto problems = onlyProblems->current();
		const auto key = QString::number(all.size())
			+ ' ' + QString::number(all.empty() ? 0 : all.back().unixMs)
			+ ' ' + QString::number(int(problems));
		if (*shown == key) {
			return;
		}
		const auto grew = shown->has_value();
		*shown = key;
		lines->clear();
		auto added = 0;
		for (const auto &line : all) {
			const auto problem = IsProblemLine(line);
			if (problems && !problem) {
				continue;
			}
			lines->add(
				object_ptr<Ui::FlatLabel>(
					lines,
					LogText(line),
					problem
						? st::pluginDangerHigh
						: line.fromApp
						? st::boxDividerLabel
						: st::boxLabel),
				st::boxRowPadding)->setSelectable(true);
			++added;
		}
		if (!added) {
			lines->add(
				object_ptr<Ui::FlatLabel>(
					lines,
					problems
						? tr::fresh_PluginLogNoProblems(tr::now)
						: tr::fresh_PluginLogEmpty(tr::now),
					st::boxDividerLabel),
				st::boxRowPadding);
		}
		if (grew || added) {
			crl::on_main(box, [=] {
				box->scrollToY(QWIDGETSIZE_MAX);
			});
		}
	};
	filter->toggledValue(
	) | rpl::on_next([=](bool value) {
		*onlyProblems = value;
		rebuild();
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
		box->closeBox();
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
		box->closeBox();
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
	return QStringList{
		Utf8(info->version),
		Utf8(info->problem),
		Utf8(info->disabledReason),
		QString::number(int(info->status)),
		QString::number(int(info->enabled)),
		QString::number(int(info->pendingUpdate)),
		QString::number(granted ? int(granted->granted.size()) : -1),
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
	const auto content = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto closedByPlugin = box->lifetime().make_state<bool>(false);
	const auto shown = box->lifetime().make_state<std::shared_ptr<Page>>();
	const auto rebuild = [=] {
		const auto &pages = GetDialogs().pages;
		const auto i = pages.find(key);
		if (i == pages.end() || !weak) {
			return;
		}
		box->setTitle(rpl::single(tr::fresh_PluginBadge(
			tr::now,
			lt_title,
			TextOf(i->second, "title"),
			lt_plugin,
			PluginName(plugin)) + suffix));
		ShowPage(content, controller, weak, plugin, &i->second, true, *shown);
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
		if (dialogs.boxes[key].data() == box.get()) {
			dialogs.boxes.erase(key);
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
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

[[nodiscard]] QString Age(crl::time opened) {
	const auto seconds = std::max<crl::time>(0, (crl::now() - opened) / 1000);
	return (seconds < 60)
		? tr::fresh_PluginsNetworkSeconds(tr::now, lt_seconds, QString::number(seconds))
		: tr::fresh_PluginsNetworkMinutes(tr::now, lt_minutes, QString::number(seconds / 60));
}


void FillNetworkBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(tr::fresh_PluginsNetwork());
	box->setWidth(st::boxWideWidth);
	const auto content = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto rebuild = [=] {
		content->clear();
		const auto snapshot = Snapshot();
		const auto connections = Connections();
		auto shown = 0;
		for (const auto &info : snapshot ? snapshot->plugins : std::vector<fgpl::PluginInfo>()) {
			const auto id = Utf8(info.id);
			const auto own = ranges::count_if(connections, [&](const ConnectionView &view) {
				return view.plugin == id;
			});
			if (!UsesNetwork(info) && !own) {
				continue;
			}
			++shown;
			Ui::AddSubsectionTitle(content, rpl::single(PluginName(info.id)));
			content->add(object_ptr<Ui::FlatLabel>(
				content,
				tr::fresh_PluginsNetworkTotal(
					tr::now,
					lt_received,
					Ui::FormatSizeText(qint64(info.stats.netReceived)),
					lt_sent,
					Ui::FormatSizeText(qint64(info.stats.netSent))),
				st::boxDividerLabel), st::boxRowPadding);
			if (!own) {
				content->add(object_ptr<Ui::FlatLabel>(
					content,
					tr::fresh_PluginsNetworkIdle(tr::now),
					st::boxDividerLabel), st::boxRowPadding);
			}
			for (const auto &view : connections) {
				if (view.plugin != id) {
					continue;
				}
				const auto endpoint = view.host + ':' + QString::number(view.port)
					+ ((view.address.isEmpty() || view.address == view.host)
						? QString()
						: u" ("_q + view.address + ')');
				const auto row = Settings::AddButtonWithLabel(
					content,
					rpl::single(tr::fresh_PluginsNetworkRow(
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
						Age(view.opened))),
					rpl::single(tr::fresh_PluginsNetworkClose(tr::now)),
					st::settingsButtonNoIcon);
				const auto connection = view.id;
				row->addClickHandler([=] {
					CloseConnection(connection);
				});
			}
			Ui::AddSkip(content);
		}
		if (!shown) {
			content->add(object_ptr<Ui::FlatLabel>(
				content,
				tr::fresh_PluginsNetworkEmpty(tr::now),
				st::boxLabel), st::boxRowPadding);
		}
	};
	rebuild();
	const auto timer = box->lifetime().make_state<base::Timer>([=] {
		rebuild();
	});
	timer->callEach(1000);
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
	};
	const auto i = icons.find(id);
	return (i != icons.end()) ? i->second : &st::menuIconLock;
}

[[nodiscard]] bool SeesChats(const fgpl::Manifest &manifest) {
	static const auto scoped = std::set<std::string>{
		"messages.read", "messages.read_deleted", "messages.history",
		"messages.send", "messages.transform_outgoing", "chats.list",
		"chat.info", "media.read", "composer.draft", "folders.list",
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
	for (const auto id : current) {
		if (const auto history = session->data().historyLoaded(PeerId(uint64(id)))) {
			peers.emplace(history);
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
			auto chosen = std::set<int64_t>();
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
				return request.id == "messages.read" && request.scope == "selected";
			})
			? 1
			: 0);
	const auto chats = std::make_shared<std::set<int64_t>>(
		(current && current->chats) ? *current->chats : std::set<int64_t>());
	const auto chatsShown = SeesChats(manifest);
	if (chatsShown) {
		Ui::AddSkip(container);
		Ui::AddSubsectionTitle(container, tr::fresh_PluginChats());
		box->addRow(object_ptr<Ui::Radiobutton>(
			box,
			chatsMode,
			0,
			tr::fresh_PluginChatsAll(tr::now),
			st::defaultBoxCheckbox));
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

void ShowComposerMenu(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		not_null<QWidget*> button) {
	static auto menu = base::unique_qptr<Ui::PopupMenu>();
	const auto session = &controller->session();
	const auto weak = base::make_weak(session);
	const auto peerId = peer->id;
	menu = base::make_unique_q<Ui::PopupMenu>(button, st::popupMenuWithIcons);
	for (const auto &entry : EntriesFor(session, "composer", peer)) {
		menu->addAction(entry.text, [=] {
			const auto strong = weak.get();
			const auto found = strong ? strong->data().peerLoaded(peerId) : nullptr;
			if (found) {
				Send(strong, entry.plugin, ClickEvent("composer", entry.menuId, found, nullptr));
			}
		}, &st::menuIconBot);
	}
	if (!menu->empty()) {
		menu->popup(QCursor::pos());
	}
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
	if (const auto open = dialogs.boxes[key].data()) {
		if (dialogs.pages[key] != *page) {
			dialogs.pages[key] = *page;
			dialogs.changes.fire_copy(key);
		}
		return;
	}
	dialogs.pages[key] = *page;
	const auto separate = Core::App().windowFor(
		Window::SeparateId(&session->account()));
	const auto window = separate ? separate : Core::App().activePrimaryWindow();
	const auto controller = window ? window->sessionController() : nullptr;
	if (!controller) {
		refuse();
		return;
	}
	auto box = Box(FillDialogBox, controller, id, account);
	dialogs.boxes[key] = box.data();
	controller->show(std::move(box));
}

void ShowNetworkPanel(not_null<Window::SessionController*> controller) {
	controller->show(Box(FillNetworkBox, controller));
}

void ForgetSettingsPages(const std::string &id) {
	auto &pages = GetPages().pages;
	for (auto i = pages.begin(); i != pages.end();) {
		i = (i->first.first == id) ? pages.erase(i) : std::next(i);
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
	if (const auto i = pages.pages.find(key); i != pages.pages.end() && i->second == page) {
		return;
	}
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
	for (const auto &entry : entries) {
		menu->addAction(entry.text, [=] {
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
		}, &st::menuIconBot);
	}
}

void AddPeerMenuItems(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId topicRootId,
		bool profile,
		Fn<void(const QString &title, Fn<void()> handler)> add) {
	if (FreshSecret::IsSecretPeerId(peer->id)) {
		return;
	}
	const auto surface = profile ? "profile" : "chat";
	const auto session = &controller->session();
	const auto weak = base::make_weak(session);
	const auto peerId = peer->id;
	for (const auto &entry : EntriesFor(session, surface, peer)) {
		add(entry.text, [=] {
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
			key.push_back(Utf8(entry.plugin + '/' + entry.menuId) + ' ' + entry.text);
		}
		if (*shown == key.join('\n')) {
			return;
		}
		*shown = key.join('\n');
		inner->clear();
		for (const auto &entry : entries) {
			FA::Ui::AddMainMenuButton(
				inner,
				rpl::single(entry.text),
				st::mainMenuButton,
				{ &st::menuIconBot }
			)->setClickedCallback([=] {
				if (const auto again = weak.get()) {
					Send(again, entry.plugin, ClickEvent("main_menu", entry.menuId, nullptr, nullptr));
				}
			});
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
	if (!snapshot || FreshSecret::IsSecretPeerId(peer->id)) {
		return result;
	}
	for (const auto &plugin : snapshot->contributions(AccountOf(session), Language())) {
		const auto grant = snapshot->grantsOf(plugin.id);
		if (grant
			&& grant->chats
			&& !grant->chats->contains(int64_t(peer->id.value))) {
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
	return result;
}

CommandResult RunCommand(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId topicRootId,
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
	Fn<void(const QString &, Fn<void()>)>) {
}

void AddMainMenuItems(
	not_null<Ui::VerticalLayout*>,
	not_null<Window::SessionController*>) {
}

CommandResult RunCommand(
		not_null<Window::SessionController*>,
		not_null<PeerData*>,
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

void ShowComposerMenu(
	not_null<Window::SessionController*>,
	not_null<PeerData*>,
	not_null<QWidget*>) {
}

void AddTrayItems(Fn<void(const QString &text, Fn<void()> handler)>) {
}

QString TrayItemsKey() {
	return QString();
}

void ShowNetworkPanel(not_null<Window::SessionController*>) {
}

void ForgetSettingsPages(const std::string &) {
}

#endif

} // namespace FreshPlugins
