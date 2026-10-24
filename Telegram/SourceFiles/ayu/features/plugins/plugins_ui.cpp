// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_ui.h"

#include "lang/lang_keys.h"
#include "ayu/features/plugins/plugins_service.h"
#include "ayu/features/plugins/plugins_transport.h"
#include "ayu/secret/secret_peer.h"
#include "core/application.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "ui/text/format_values.h"
#include "window/window_controller.h"
#include "ayu/ui/boxes/plugin_install_box.h"
#include "ayu/ui/fagram/fagram_nav_drawer.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
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
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/manager.h"
#include "fgpl/permissions.h"
#endif

#include <map>

namespace FreshPlugins {

#ifdef FRESHGRAM_PLUGINS

namespace {

namespace cbor = fgpl::cbor;

struct Entry {
	std::string plugin;
	std::string menuId;
	QString text;
};

struct Pages {
	std::map<std::pair<std::string, std::string>, cbor::Value> pages;
	rpl::event_stream<std::string> changes;
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
		const char *surface) {
	auto result = std::vector<Entry>();
	const auto manager = Manager();
	if (!manager) {
		return result;
	}
	for (const auto &plugin : manager->contributions(AccountOf(session), Language())) {
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
	const auto manager = Manager();
	if (!manager || !manager->send(plugin, AccountOf(session), event)) {
		::Ui::Toast::Show(tr::fresh_PluginNotRunning(tr::now));
	}
}

[[nodiscard]] cbor::Value ClickEvent(
		const char *surface,
		const std::string &menuId,
		PeerData *peer,
		HistoryItem *item) {
	auto map = cbor::Map();
	map.push_back({ "type", cbor::Value::FromText("menu.click") });
	map.push_back({ "surface", cbor::Value::FromText(surface) });
	map.push_back({ "id", cbor::Value::FromText(menuId) });
	if (peer) {
		map.push_back({ "chat", Number(int64_t(peer->id.value)) });
		map.push_back({ "secret", Flag(FreshSecret::IsSecretPeerId(peer->id)) });
	}
	if (item) {
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
	if (const auto i = pages.find({ id, account }); i != pages.end()) {
		return &i->second;
	}
	for (const auto &[key, page] : pages) {
		if (key.first == id) {
			return &page;
		}
	}
	return nullptr;
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

void BuildSettingsContent(
		not_null<Ui::VerticalLayout*> content,
		not_null<Window::SessionController*> controller,
		not_null<Main::Session*> session,
		const std::string &plugin,
		const cbor::Value &page,
		bool dialog) {
	const auto weak = base::make_weak(session);
	const auto change = dialog ? "dialog.change" : "settings.change";
	const auto click = dialog ? "dialog.click" : "settings.click";
	const auto items = page.find("items");
	for (const auto &item : items ? *items->asArray() : cbor::Array()) {
		const auto type = TextOf(item, "type");
		const auto itemId = item.find("id") && item.find("id")->asText()
			? *item.find("id")->asText()
			: std::string();
		const auto label = TextOf(item, "label");
		if (type == u"text"_q) {
			content->add(
				object_ptr<Ui::FlatLabel>(content, TextOf(item, "text"), st::boxLabel),
				st::boxRowPadding);
			Ui::AddSkip(content);
		} else if (type == u"divider"_q) {
			Ui::AddDivider(content);
		} else if (type == u"toggle"_q) {
			const auto on = content->lifetime().make_state<rpl::variable<bool>>(
				item.find("value") && item.find("value")->asBool().value_or(false));
			const auto button = Settings::AddButtonWithIcon(
				content,
				rpl::single(label),
				st::settingsButtonNoIcon);
			button->toggleOn(on->value(), true);
			button->addClickHandler([=] {
				*on = !on->current();
				if (const auto strong = weak.get()) {
					SendSetting(strong, plugin, change, itemId, Flag(on->current()));
				}
			});
		} else if (type == u"button"_q) {
			const auto button = Settings::AddButtonWithIcon(
				content,
				rpl::single(label),
				st::settingsButtonNoIcon);
			button->addClickHandler([=] {
				if (const auto strong = weak.get()) {
					SendSetting(strong, plugin, click, itemId, std::nullopt);
				}
			});
		} else if (type == u"input"_q) {
			const auto field = content->add(
				object_ptr<Ui::InputField>(
					content,
					st::defaultInputField,
					rpl::single(label),
					TextOf(item, "value")),
				st::boxRowPadding);
			const auto timer = field->lifetime().make_state<base::Timer>();
			timer->setCallback([=] {
				if (const auto strong = weak.get()) {
					SendSetting(
						strong,
						plugin,
						change,
						itemId,
						Text(field->getLastText()));
				}
			});
			field->changes() | rpl::on_next([=] {
				timer->callOnce(600);
			}, field->lifetime());
			Ui::AddSkip(content);
		} else if (type == u"select"_q) {
			auto values = std::vector<std::string>();
			auto labels = std::vector<QString>();
			const auto current = content->lifetime().make_state<rpl::variable<QString>>();
			const auto selected = content->lifetime().make_state<int>(0);
			const auto chosen = TextOf(item, "value").toStdString();
			const auto options = item.find("options");
			for (const auto &option : options ? *options->asArray() : cbor::Array()) {
				const auto value = option.find("value");
				if (!value || !value->asText()) {
					continue;
				}
				if (*value->asText() == chosen) {
					*selected = int(values.size());
				}
				values.push_back(*value->asText());
				labels.push_back(TextOf(option, "label"));
			}
			if (labels.empty()) {
				continue;
			}
			*current = labels[*selected];
			const auto button = Settings::AddButtonWithLabel(
				content,
				rpl::single(label),
				current->value(),
				st::settingsButtonNoIcon);
			button->addClickHandler([=] {
				controller->show(Box(SingleChoiceBox, SingleChoiceBoxArgs{
					.title = rpl::single(label),
					.options = labels,
					.initialSelection = *selected,
					.callback = [=](int index) {
						*selected = index;
						*current = labels[index];
						if (const auto strong = weak.get()) {
							SendSetting(
								strong,
								plugin,
								change,
								itemId,
								cbor::Value::FromText(values[index]));
						}
					},
				}));
			});
		}
	}
}

void FillSettingsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		std::string plugin,
		std::string account,
		QString pluginName) {
	box->setWidth(st::boxWideWidth);
	const auto page = PageOf(plugin, account);
	const auto title = page ? TextOf(*page, "title") : QString();
	box->setTitle(rpl::single(tr::fresh_PluginBadge(
		tr::now,
		lt_title,
		title,
		lt_plugin,
		pluginName)));
	const auto content = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto rebuild = [=] {
		content->clear();
		if (const auto current = PageOf(plugin, account)) {
			BuildSettingsContent(content, controller, &controller->session(), plugin, *current, false);
		}
	};
	rebuild();
	GetPages().changes.events(
	) | rpl::filter([=](const std::string &id) {
		return id == plugin;
	}) | rpl::on_next([=](const std::string &) {
		Ui::PostponeCall(content, rebuild);
	}, content->lifetime());
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

void FillLogBox(not_null<Ui::GenericBox*> box, std::string plugin) {
	box->setTitle(tr::fresh_PluginLog());
	auto text = QString();
	if (const auto manager = Manager()) {
		for (const auto &line : manager->log(plugin)) {
			text += Utf8(line) + '\n';
		}
	}
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		text.isEmpty() ? tr::fresh_PluginLogEmpty(tr::now) : text.trimmed(),
		st::boxLabel));
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

void FillDetailsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		std::string plugin) {
	const auto manager = Manager();
	const auto parsed = manager ? manager->manifest(plugin) : nullptr;
	if (!manager || !parsed) {
		box->closeBox();
		return;
	}
	const auto account = AccountOf(&controller->session());
	const auto info = [&] {
		for (auto &entry : manager->list()) {
			if (entry.id == plugin) {
				return entry;
			}
		}
		return fgpl::PluginInfo();
	}();
	const auto name = Utf8(parsed->name.get(Language()));
	box->setWidth(st::boxWideWidth);
	box->setTitle(rpl::single(name + ' ' + Utf8(info.version)));

	const auto label = [&](const QString &text, const style::FlatLabel &st = st::boxLabel) {
		if (!text.isEmpty()) {
			box->addRow(object_ptr<Ui::FlatLabel>(box, text, st));
		}
	};
	label(Utf8(parsed->description.get(Language())));
	label(info.signedPackage
		? tr::fresh_PluginSignedBy(tr::now, lt_publisher, Utf8(info.publisher))
		: tr::fresh_PluginUnsigned(tr::now), st::boxDividerLabel);
	if (info.pendingUpdate) {
		label(tr::fresh_PluginPending(tr::now, lt_version, Utf8(info.version)), st::boxDividerLabel);
	}
	if (info.outdated) {
		label(tr::fresh_PluginOutdated(tr::now, lt_version, Utf8(info.testedWith)), st::boxDividerLabel);
	}
	if (!parsed->messagesFrom.empty()) {
		label(AcceptsFrom(parsed->messagesFrom), st::boxDividerLabel);
	}
	if (!info.disabledReason.empty()) {
		label(tr::fresh_PluginStatusDisabled(tr::now, lt_reason, Utf8(info.disabledReason)), st::boxDividerLabel);
	}
	for (const auto &entry : manager->contributions(account, Language())) {
		if (entry.id == plugin) {
			label(ContributionSummary(entry), st::boxDividerLabel);
		}
	}
	label(tr::fresh_PluginStats(
		tr::now,
		lt_cpu,
		QString::number(info.stats.cpuMs / 1000.,'f', 1),
		lt_memory,
		Ui::FormatSizeText(qint64(info.stats.memoryBytes)),
		lt_received,
		Ui::FormatSizeText(qint64(info.stats.netReceived)),
		lt_sent,
		Ui::FormatSizeText(qint64(info.stats.netSent))), st::boxDividerLabel);
	if (info.stats.unloads) {
		label(tr::fresh_PluginUnloads(
			tr::now,
			lt_times,
			QString::number(info.stats.unloads)), st::boxDividerLabel);
	}

	const auto granted = manager->grants(plugin, account);
	auto checks = std::make_shared<std::vector<std::pair<std::string, Ui::Checkbox*>>>();
	Ui::AddSubsectionTitle(box->verticalLayout(), tr::fresh_PluginPermissions());
	label(tr::fresh_PluginPermissionsAllAccounts(tr::now), st::boxDividerLabel);
	if (parsed->permissions.empty()) {
		label(tr::fresh_PluginNoPermissions(tr::now), st::boxDividerLabel);
	}
	for (const auto &request : parsed->permissions) {
		auto text = PermissionLabel(request.id);
		if (request.required) {
			text = tr::fresh_PluginPermissionRequired(tr::now, lt_permission, text);
		}
		const auto check = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			text,
			request.required || (granted && granted->granted.contains(request.id))));
		if (request.required) {
			check->setDisabled(true);
		}
		checks->emplace_back(request.id, check);
		AddPermissionDanger(
			box->verticalLayout(),
			request.id,
			request.scope,
			Utf8(request.reason.get(Language())));
	}

	const auto container = box->verticalLayout();
	Ui::AddSkip(container);
	if (parsed->settingsPage) {
		Settings::AddButtonWithIcon(
			container,
			tr::fresh_PluginSettingsButton(),
			st::settingsButtonNoIcon
		)->addClickHandler([=] {
			ShowPluginSettings(controller, Utf8(plugin));
		});
	}
	Settings::AddButtonWithIcon(
		container,
		tr::fresh_PluginLog(),
		st::settingsButtonNoIcon
	)->addClickHandler([=] {
		controller->show(Box(FillLogBox, plugin));
	});
	Settings::AddButtonWithIcon(
		container,
		tr::fresh_PluginsNetwork(),
		st::settingsButtonNoIcon
	)->addClickHandler([=] {
		ShowNetworkPanel(controller);
	});
	Settings::AddButtonWithIcon(
		container,
		tr::fresh_PluginUpdateFromFile(),
		st::settingsButtonNoIcon
	)->addClickHandler([=] {
		AyuUi::ChoosePluginFile(controller);
	});
	const auto weak = base::make_weak(box);
	Settings::AddButtonWithIcon(
		container,
		tr::fresh_PluginsRemove(),
		st::settingsAttentionButton
	)->addClickHandler([=] {
		controller->show(Ui::MakeConfirmBox({
			.text = tr::fresh_PluginsRemoveConfirmation(
				tr::now,
				lt_name,
				::Ui::Text::Bold(name),
				tr::marked),
			.confirmed = [=](Fn<void()> &&close) {
				if (const auto again = Manager()) {
					again->uninstall(plugin, true);
					ForgetSettingsPages(plugin);
					NotifyListChanged();
				}
				close();
				if (const auto strong = weak.get()) {
					strong->closeBox();
				}
			},
			.confirmText = tr::fresh_PluginsRemove(),
		}));
	});

	box->addButton(tr::fresh_PluginApply(), [=] {
		const auto again = Manager();
		if (!again) {
			return;
		}
		auto decision = fgpl::GrantDecision();
		if (const auto previous = again->grants(plugin, account)) {
			decision.chats = previous->chats;
		}
		for (const auto &[id, check] : *checks) {
			if (check->checked()) {
				decision.granted.insert(id);
			}
		}
		again->grant(plugin, account, decision);
		again->startEnabled(account);
		NotifyListChanged();
		controller->showToast(tr::fresh_PluginPermissionsSaved(tr::now));
		box->closeBox();
	});
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
	const auto manager = Manager();
	const auto parsed = manager ? manager->manifest(id) : nullptr;
	return parsed ? Utf8(parsed->name.get(Language())) : Utf8(id);
}

void FillDialogBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		std::string plugin,
		std::string account) {
	const auto key = std::make_pair(plugin, account);
	const auto session = SessionFor(account);
	box->setWidth(st::boxWideWidth);
	const auto content = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto closedByPlugin = box->lifetime().make_state<bool>(false);
	const auto rebuild = [=] {
		content->clear();
		const auto &pages = GetDialogs().pages;
		const auto i = pages.find(key);
		if (i == pages.end() || !session) {
			return;
		}
		box->setTitle(rpl::single(tr::fresh_PluginBadge(
			tr::now,
			lt_title,
			TextOf(i->second, "title"),
			lt_plugin,
			PluginName(plugin))));
		BuildSettingsContent(content, controller, session, plugin, i->second, true);
	};
	rebuild();
	GetDialogs().changes.events(
	) | rpl::filter([=](const auto &changed) {
		return changed == key;
	}) | rpl::on_next([=] {
		if (!GetDialogs().pages.contains(key)) {
			*closedByPlugin = true;
			box->closeBox();
		} else {
			Ui::PostponeCall(content, rebuild);
		}
	}, content->lifetime());
	box->boxClosing() | rpl::on_next([=] {
		if (!*closedByPlugin) {
			GetDialogs().pages.erase(key);
			if (const auto strong = SessionFor(account)) {
				const auto manager = Manager();
				if (manager) {
					manager->send(plugin, account, cbor::Value::FromMap({
						{ "type", cbor::Value::FromText("dialog.close") },
					}));
				}
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

[[nodiscard]] bool UsesNetwork(const fgpl::PluginInfo &info, const std::string &account) {
	const auto manager = Manager();
	const auto grant = manager ? manager->grants(info.id, account) : std::nullopt;
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

void FillNetworkBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(tr::fresh_PluginsNetwork());
	box->setWidth(st::boxWideWidth);
	const auto account = AccountOf(&controller->session());
	const auto content = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		style::margins());
	const auto rebuild = [=] {
		content->clear();
		const auto manager = Manager();
		const auto connections = Connections();
		auto shown = 0;
		for (const auto &info : manager ? manager->list() : std::vector<fgpl::PluginInfo>()) {
			const auto id = Utf8(info.id);
			const auto own = ranges::count_if(connections, [&](const ConnectionView &view) {
				return view.plugin == id;
			});
			if (!UsesNetwork(info, account) && !own) {
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

QString AcceptsFrom(const std::vector<std::string> &senders) {
	auto names = QStringList();
	for (const auto &sender : senders) {
		names.push_back((sender == "*")
			? tr::fresh_PluginAcceptsAny(tr::now)
			: QString::fromStdString(sender));
	}
	return tr::fresh_PluginAcceptsFrom(tr::now, lt_list, names.join(u", "_q));
}

void AddPermissionDanger(
		not_null<Ui::VerticalLayout*> layout,
		const std::string &id,
		const std::string &scope,
		const QString &reason) {
	const auto danger = fgpl::PermissionDanger(id, scope);
	const auto level = (danger == fgpl::Danger::High)
		? tr::fresh_PluginDangerHigh(tr::now)
		: (danger == fgpl::Danger::Medium)
		? tr::fresh_PluginDangerMedium(tr::now)
		: tr::fresh_PluginDangerLow(tr::now);
	const auto &style = (danger == fgpl::Danger::High)
		? st::pluginDangerHigh
		: (danger == fgpl::Danger::Medium)
		? st::pluginDangerMedium
		: st::pluginDangerLow;
	layout->add(
		object_ptr<Ui::FlatLabel>(
			layout,
			reason.isEmpty() ? level : (level + u" · "_q + reason),
			style),
		st::boxRowPadding);
}

QString PermissionLabel(const std::string &id) {
	if (id == "net.https") {
		return tr::fresh_PermissionNetHttps(tr::now);
	} else if (id == "net.tcp") {
		return tr::fresh_PermissionNetTcp(tr::now);
	} else if (id == "net.udp") {
		return tr::fresh_PermissionNetUdp(tr::now);
	} else if (id == "plugins.message") {
		return tr::fresh_PermissionPluginsMessage(tr::now);
	}
	return Utf8(id);
}

bool HasComposerItems(not_null<Main::Session*> session) {
	return !EntriesFor(session, "composer").empty();
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
	for (const auto &entry : EntriesFor(session, "composer")) {
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
	dialogs.pages[key] = *page;
	if (const auto open = dialogs.boxes[key].data()) {
		dialogs.changes.fire_copy(key);
		return;
	}
	const auto window = Core::App().activePrimaryWindow();
	const auto controller = window ? window->sessionController() : nullptr;
	if (!controller || !SessionFor(account)) {
		dialogs.pages.erase(key);
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
	pages.pages[{ id, account }] = page;
	pages.changes.fire_copy(id);
}

void AddMessageMenuItems(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item) {
	const auto peer = item->history()->peer;
	if (item->isService() || FreshSecret::IsSecretPeerId(peer->id)) {
		return;
	}
	const auto session = &item->history()->session();
	const auto entries = EntriesFor(session, "message");
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
		});
	}
}

void AddPeerMenuItems(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		bool profile,
		Fn<void(const QString &title, Fn<void()> handler)> add) {
	if (FreshSecret::IsSecretPeerId(peer->id)) {
		return;
	}
	const auto surface = profile ? "profile" : "chat";
	const auto session = &controller->session();
	const auto weak = base::make_weak(session);
	const auto peerId = peer->id;
	for (const auto &entry : EntriesFor(session, surface)) {
		add(entry.text, [=] {
			const auto strong = weak.get();
			const auto found = strong ? strong->data().peerLoaded(peerId) : nullptr;
			if (found) {
				Send(strong, entry.plugin, ClickEvent(surface, entry.menuId, found, nullptr));
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
	const auto rebuild = [=] {
		inner->clear();
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		for (const auto &entry : EntriesFor(strong, "main_menu")) {
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

bool TryRunCommand(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		const QString &text) {
	const auto manager = Manager();
	if (!manager || !text.startsWith('/')
		|| FreshSecret::IsSecretPeerId(peer->id)
		|| (peer->isUser() && peer->asUser()->isBot())) {
		return false;
	}
	const auto trimmed = text.trimmed();
	const auto space = trimmed.indexOf(QRegularExpression(u"\\s"_q));
	const auto name = trimmed.mid(1, space < 0 ? -1 : space - 1).toLower();
	if (name.isEmpty() || name.contains('@')) {
		return false;
	}
	const auto args = (space < 0) ? QString() : trimmed.mid(space + 1).trimmed();
	const auto session = &controller->session();
	for (const auto &plugin : manager->contributions(AccountOf(session), Language())) {
		for (const auto &command : plugin.commands) {
			if (Utf8(command.name) != name) {
				continue;
			}
			Send(session, plugin.id, cbor::Value::FromMap({
				{ "type", cbor::Value::FromText("command") },
				{ "name", Text(name) },
				{ "args", Text(args) },
				{ "chat", Number(int64_t(peer->id.value)) },
				{ "secret", Flag(false) },
			}));
			return true;
		}
	}
	return false;
}

void ShowPluginSettings(
		not_null<Window::SessionController*> controller,
		const QString &pluginId) {
	const auto manager = Manager();
	const auto id = pluginId.toStdString();
	const auto account = AccountOf(&controller->session());
	if (!manager || !PageOf(id, account)) {
		controller->showToast(tr::fresh_PluginSettingsUnavailable(tr::now));
		return;
	}
	const auto parsed = manager->manifest(id);
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
	bool,
	Fn<void(const QString &, Fn<void()>)>) {
}

void AddMainMenuItems(
	not_null<Ui::VerticalLayout*>,
	not_null<Window::SessionController*>) {
}

bool TryRunCommand(
		not_null<Window::SessionController*>,
		not_null<PeerData*>,
		const QString &) {
	return false;
}

void ShowPluginSettings(not_null<Window::SessionController*>, const QString &) {
}

void ShowPluginDetails(not_null<Window::SessionController*>, const QString &) {
}

QString PermissionLabel(const std::string &id) {
	return QString::fromStdString(id);
}

QString AcceptsFrom(const std::vector<std::string> &) {
	return QString();
}

void AddPermissionDanger(
	not_null<Ui::VerticalLayout*>,
	const std::string &,
	const std::string &,
	const QString &) {
}

bool HasComposerItems(not_null<Main::Session*>) {
	return false;
}

void ShowComposerMenu(
	not_null<Window::SessionController*>,
	not_null<PeerData*>,
	not_null<QWidget*>) {
}

void AddTrayItems(Fn<void(const QString &text, Fn<void()> handler)>) {
}

void ShowNetworkPanel(not_null<Window::SessionController*>) {
}

void ForgetSettingsPages(const std::string &) {
}

#endif

} // namespace FreshPlugins
