// This is the source code of freshGram.
#include "ayu/ui/boxes/plugins_proxies_box.h"

#include "lang_auto.h"
#include "ayu/ayu_settings.h"
#include "base/flat_map.h"
#include "base/unique_qptr.h"
#include "boxes/connection_box.h"
#include "lang/lang_keys.h"
#include "styles/style_boxes.h"
#include "styles/style_connection_box.h"
#include "styles/style_layers.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

namespace AyuUi {
namespace {

enum class Mode {
	Disabled,
	Enabled,
};

struct Entry {
	int id = 0;
	PluginsProxy data;
	bool deleted = false;
};

struct State {
	std::vector<Entry> list;
	int idCounter = 0;
	int lastSelectedId = 0;
	base::flat_map<int, base::unique_qptr<ProxyRow>> rows;
	std::shared_ptr<Ui::RadioenumGroup<Mode>> mode;
	QPointer<Ui::VerticalLayout> wrap;
	base::unique_qptr<Ui::RpWidget> noRows;
};

[[nodiscard]] MTP::ProxyData ProxyDataOf(const PluginsProxy &proxy) {
	auto result = MTP::ProxyData();
	if (!proxy.valid()) {
		return result;
	}
	result.type = (proxy.type == PluginsProxy::Type::Http)
		? MTP::ProxyData::Type::Http
		: MTP::ProxyData::Type::Socks5;
	result.host = QString::fromStdString(proxy.host);
	result.port = uint32(proxy.port);
	result.user = QString::fromStdString(proxy.user);
	result.password = QString::fromStdString(proxy.password);
	return result;
}

[[nodiscard]] PluginsProxy ProxyOf(const MTP::ProxyData &data) {
	auto result = PluginsProxy();
	result.type = (data.type == MTP::ProxyData::Type::Http)
		? PluginsProxy::Type::Http
		: PluginsProxy::Type::Socks5;
	result.host = data.host.toStdString();
	result.port = int(data.port);
	result.user = data.user.toStdString();
	result.password = data.password.toStdString();
	return result;
}

void FillBox(not_null<Ui::GenericBox*> box) {
	const auto state = box->lifetime().make_state<State>();
	auto &settings = AyuSettings::getInstance();

	for (const auto &proxy : settings.pluginsProxyList()) {
		state->list.push_back({ ++state->idCounter, proxy });
	}
	const auto current = settings.pluginsProxy();
	if (current.valid() && ranges::none_of(state->list, [&](const Entry &e) {
		return e.data.sameEndpoint(current);
	})) {
		state->list.push_back({ ++state->idCounter, current });
	}
	for (auto &entry : state->list) {
		entry.data.enabled = false;
	}

	const auto find = [=](int id) {
		return ranges::find(state->list, id, &Entry::id);
	};
	const auto isSelected = [](const PluginsProxy &data) {
		const auto &selected = AyuSettings::getInstance().pluginsProxy();
		return selected.valid() && selected.sameEndpoint(data);
	};
	const auto commit = [=] {
		auto list = std::vector<PluginsProxy>();
		for (const auto &entry : state->list) {
			if (!entry.deleted) {
				list.push_back(entry.data);
			}
		}
		AyuSettings::getInstance().setPluginsProxyList(std::move(list));
	};
	const auto viewOf = [=](const Entry &entry) {
		const auto selected = !entry.deleted && isSelected(entry.data);
		const auto enabled = AyuSettings::getInstance().pluginsProxy().enabled;
		return ProxyRow::View{
			.id = entry.id,
			.type = (entry.data.type == PluginsProxy::Type::Http)
				? u"HTTP"_q
				: u"SOCKS5"_q,
			.host = QString::fromStdString(entry.data.host),
			.port = uint32(entry.data.port),
			.selected = selected,
			.deleted = entry.deleted,
			.state = (selected && enabled)
				? ProxyRow::State::Online
				: ProxyRow::State::NotTested,
		};
	};
	const auto refreshRows = [=] {
		for (const auto &entry : state->list) {
			const auto i = state->rows.find(entry.id);
			if (i != state->rows.end()) {
				i->second->updateFields(viewOf(entry));
			}
		}
	};
	const auto select = [=](const PluginsProxy &data, bool enabled) {
		auto value = data;
		value.enabled = enabled;
		AyuSettings::getInstance().setPluginsProxy(std::move(value));
	};

	const auto addNewBox = std::make_shared<Fn<void()>>();
	const auto showEditBox = std::make_shared<Fn<void(int)>>();
	const auto apply = [=](int id) {
		const auto i = find(id);
		if (i == state->list.end() || i->deleted) {
			return;
		}
		select(i->data, true);
		state->mode->setValue(Mode::Enabled);
		refreshRows();
	};
	const auto ensureRow = [=](const Entry &entry) {
		if (state->rows.contains(entry.id)) {
			return;
		}
		const auto id = entry.id;
		const auto wrap = state->wrap.data();
		const auto &[i, ok] = state->rows.emplace(id, nullptr);
		i->second.reset(wrap->insert(
			0,
			object_ptr<ProxyRow>(wrap, viewOf(entry))));
		const auto row = i->second.get();
		state->noRows.reset();
		row->deleteClicks() | rpl::on_next([=] {
			const auto i = find(id);
			if (i == state->list.end() || i->deleted) {
				return;
			}
			i->deleted = true;
			if (isSelected(i->data)) {
				state->lastSelectedId = id;
				AyuSettings::getInstance().setPluginsProxy({});
				state->mode->setValue(Mode::Disabled);
			}
			commit();
			refreshRows();
		}, row->lifetime());
		row->restoreClicks() | rpl::on_next([=] {
			const auto i = find(id);
			if (i == state->list.end() || !i->deleted) {
				return;
			}
			i->deleted = false;
			if (!AyuSettings::getInstance().pluginsProxy().valid()
				&& state->lastSelectedId == id) {
				select(i->data, false);
			}
			commit();
			refreshRows();
		}, row->lifetime());
		row->editClicks() | rpl::on_next([=] {
			(*showEditBox)(id);
		}, row->lifetime());
		row->clicks() | rpl::on_next([=] {
			apply(id);
		}, row->lifetime());
	};
	const auto addOrApply = [=](const PluginsProxy &proxy) {
		const auto i = ranges::find_if(state->list, [&](const Entry &e) {
			return e.data.sameEndpoint(proxy);
		});
		if (i != state->list.end()) {
			i->deleted = false;
			commit();
			apply(i->id);
			return;
		}
		auto entry = Entry{ ++state->idCounter, proxy };
		entry.data.enabled = false;
		state->list.push_back(entry);
		commit();
		ensureRow(state->list.back());
		apply(entry.id);
	};
	*addNewBox = [=] {
		box->uiShow()->showBox(ProxyEditBox(
			MTP::ProxyData(),
			{ MTP::ProxyData::Type::Socks5, MTP::ProxyData::Type::Http },
			[=](const MTP::ProxyData &data) {
				addOrApply(ProxyOf(data));
			}));
	};
	*showEditBox = [=](int id) {
		const auto i = find(id);
		if (i == state->list.end()) {
			return;
		}
		box->uiShow()->showBox(ProxyEditBox(
			ProxyDataOf(i->data),
			{ MTP::ProxyData::Type::Socks5, MTP::ProxyData::Type::Http },
			[=](const MTP::ProxyData &data) {
				const auto i = find(id);
				if (i == state->list.end()) {
					return;
				}
				auto result = ProxyOf(data);
				const auto same = ranges::find_if(
					state->list,
					[&](const Entry &e) {
						return (e.id != id) && e.data.sameEndpoint(result);
					});
				const auto wasSelected = isSelected(i->data);
				if (same != state->list.end()) {
					const auto target = same->id;
					state->rows.erase(id);
					state->list.erase(i);
					find(target)->deleted = false;
					commit();
					apply(target);
					return;
				}
				i->deleted = false;
				i->data = result;
				if (wasSelected) {
					select(result, true);
				}
				commit();
				apply(id);
			}));
	};

	box->setTitle(tr::fresh_PluginsProxy());
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();

	const auto selected = settings.pluginsProxy();
	state->mode = std::make_shared<Ui::RadioenumGroup<Mode>>(
		(selected.enabled && selected.valid())
			? Mode::Enabled
			: Mode::Disabled);
	container->add(
		object_ptr<Ui::Radioenum<Mode>>(
			container,
			state->mode,
			Mode::Disabled,
			tr::lng_proxy_disable(tr::now)),
		st::proxyUsePadding);
	container->add(
		object_ptr<Ui::Radioenum<Mode>>(
			container,
			state->mode,
			Mode::Enabled,
			tr::lng_proxy_use_custom(tr::now)),
		st::proxyUsePadding);
	Ui::AddSkip(container);

	state->wrap = container->add(object_ptr<Ui::VerticalLayout>(container));
	container->add(object_ptr<Ui::FixedHeightWidget>(
		container,
		st::proxyRowPadding.bottom()));

	for (const auto &entry : state->list) {
		ensureRow(entry);
	}
	if (state->rows.empty()) {
		state->noRows.reset(state->wrap->add(
			object_ptr<Ui::FlatLabel>(
				state->wrap,
				tr::lng_proxy_description(tr::now),
				st::proxyEmptyListLabel),
			st::proxyEmptyListPadding));
	}
	commit();

	state->mode->setChangedCallback([=](Mode value) {
		auto proxy = AyuSettings::getInstance().pluginsProxy();
		if (value == Mode::Disabled) {
			proxy.enabled = false;
			AyuSettings::getInstance().setPluginsProxy(std::move(proxy));
			refreshRows();
			return;
		}
		if (!proxy.valid() || ranges::none_of(state->list, [&](const Entry &e) {
			return !e.deleted && e.data.sameEndpoint(proxy);
		})) {
			auto last = state->list.size();
			while (last && state->list[last - 1].deleted) {
				--last;
			}
			if (!last) {
				state->mode->setValue(Mode::Disabled);
				(*addNewBox)();
				return;
			}
			proxy = state->list[last - 1].data;
		}
		select(proxy, true);
		refreshRows();
	});

	box->addButton(tr::lng_proxy_add(), [=] { (*addNewBox)(); });
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace

void ShowPluginsProxiesBox(not_null<Window::SessionController*> controller) {
	controller->show(Box(FillBox));
}

} // namespace AyuUi
