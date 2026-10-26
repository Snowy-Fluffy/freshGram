// This is the source code of freshGram.
#include "ayu/ui/settings/plugins_settings.h"

#include "lang_auto.h"
#include "ayu/ayu_settings.h"
#include "ayu/features/plugins/plugins_runtime.h"
#include "ayu/features/plugins/plugins_service.h"
#include "ayu/features/plugins/plugins_ui.h"
#include "ayu/ui/boxes/plugin_install_box.h"
#include "core/file_utilities.h"
#include "mainwindow.h"
#include "lang/lang_keys.h"
#include "settings/settings_common.h"
#include "settings/settings_notifications_common.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/manager.h"
#endif

namespace AyuUi {
namespace {

constexpr auto kPendingToggleMs = crl::time(3000);

[[nodiscard]] QString StateText(FreshPlugins::State state) {
	switch (state) {
	case FreshPlugins::State::Unavailable:
		return tr::fresh_PluginsUnavailable(tr::now);
	case FreshPlugins::State::NoSandbox:
		return tr::fresh_PluginsNoSandbox(tr::now);
	case FreshPlugins::State::NoRuntime:
		return tr::fresh_PluginsNoRuntime(tr::now);
	case FreshPlugins::State::NoKey:
		return tr::fresh_PluginsNoKey(tr::now);
	case FreshPlugins::State::Disabled:
	case FreshPlugins::State::Ready:
		return tr::fresh_PluginsDescription(tr::now);
	}
	return QString();
}

#ifdef FRESHGRAM_PLUGINS

[[nodiscard]] QString ShortReason(const std::string &reason) {
	constexpr auto kMaxReason = 48;
	const auto text = QString::fromStdString(reason);
	return (text.size() > kMaxReason)
		? (text.left(kMaxReason - 1).trimmed() + QChar(0x2026))
		: text;
}

[[nodiscard]] QString StatusLine(const fgpl::PluginInfo &info) {
	auto parts = QStringList{ QString::fromStdString(info.version) };
	if (!info.problem.empty()) {
		parts.push_back(tr::fresh_PluginBroken(
			tr::now,
			lt_reason,
			ShortReason(info.problem)));
	} else if (!info.disabledReason.empty()) {
		parts.push_back(tr::fresh_PluginStatusDisabled(
			tr::now,
			lt_reason,
			ShortReason(info.disabledReason)));
	} else if (info.status == fgpl::PluginStatus::Running) {
		parts.push_back(tr::fresh_PluginStatusRunning(tr::now));
	} else if (!info.enabled) {
		parts.push_back(tr::fresh_PluginStatusOff(tr::now));
	} else if (info.retryInMs > 0) {
		parts.push_back(tr::fresh_PluginStatusRetry(
			tr::now,
			lt_seconds,
			QString::number((info.retryInMs + 999) / 1000),
			lt_reason,
			ShortReason(info.lastFailure)));
	} else if (!info.lastFailure.empty()) {
		parts.push_back(tr::fresh_PluginStatusFailed(
			tr::now,
			lt_reason,
			ShortReason(info.lastFailure)));
	} else if (info.onDemand) {
		parts.push_back(tr::fresh_PluginStatusOnDemand(tr::now));
	} else {
		parts.push_back(tr::fresh_PluginStatusStopped(tr::now));
	}
	if (info.outdated) {
		parts.push_back(tr::fresh_PluginOutdatedShort(tr::now));
	}
	return parts.join(u" · "_q);
}

// WHY: the card background is painted by the outer layout and does not notice rebuilt rows.
void RepaintCards(not_null<QWidget*> widget) {
	auto target = widget.get();
	for (auto parent = widget->parentWidget(); parent; parent = parent->parentWidget()) {
		if (parent->property("fa_cards").toBool()) {
			target = parent;
		}
	}
	target->update();
}

struct PendingToggle {
	bool value = false;
	crl::time since = 0;
};

struct PluginRow {
	std::shared_ptr<rpl::variable<QString>> status;
	Ui::ToggleView *check = nullptr;
	std::shared_ptr<PendingToggle> pending;
};

[[nodiscard]] PluginRow AddPluginRow(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		const fgpl::PluginInfo &info) {
	const auto id = info.id;
	const auto broken = !info.problem.empty();
	const auto name = info.name.empty()
		? QString::fromStdString(info.id)
		: QString::fromStdString(info.name);
	const auto row = container->add(object_ptr<Ui::VerticalLayout>(container));
	const auto status = std::make_shared<rpl::variable<QString>>(StatusLine(info));
	const auto pending = std::make_shared<PendingToggle>();
	const auto [button, toggle, checkView] = Settings::SetupSplitToggle(
		row,
		rpl::single(name),
		&st::menuIconBot,
		info.enabled && !broken,
		status->value());
	button->setClickedCallback([=] {
		FreshPlugins::ShowPluginDetails(controller, QString::fromStdString(id));
	});
	toggle->setClickedCallback([=, view = checkView.get()] {
		if (broken) {
			controller->showToast(tr::fresh_PluginBrokenToast(tr::now));
			return;
		}
		const auto enable = !view->checked();
		view->setChecked(enable, anim::type::normal);
		*pending = { enable, crl::now() };
		FreshPlugins::Post([=](not_null<fgpl::PluginManager*> manager) {
			manager->setEnabled(id, enable);
		});
	});
	return { status, checkView.get(), pending };
}

#endif

[[nodiscard]] QString SizeText(qint64 bytes) {
	return QString::number(std::max<qint64>(1, bytes / (1024 * 1024))) + u" MB"_q;
}

[[nodiscard]] QString RuntimeLabel(const FreshPlugins::RuntimeStatus &status) {
	using Stage = FreshPlugins::RuntimeStage;
	switch (status.stage) {
	case Stage::Checking:
		return tr::fresh_PluginsChecking(tr::now);
	case Stage::Downloading:
		return tr::fresh_PluginsDownloading(
			tr::now,
			lt_percent,
			QString::number(int(status.progress * 100)) + '%');
	case Stage::Installing:
		return tr::fresh_PluginsInstalling(tr::now);
	case Stage::Idle:
		break;
	}
	if (!status.error.isEmpty()) {
		return status.error;
	}
	auto result = status.installed
		? tr::fresh_PluginsVersions(
			tr::now,
			lt_wasmtime,
			status.wasmtime,
			lt_python,
			status.python)
		: tr::fresh_PluginsNotInstalled(tr::now);
	if (status.updateAvailable && status.installed) {
		result = tr::fresh_PluginsUpdateAvailable(
			tr::now,
			lt_wasmtime,
			status.remoteWasmtime,
			lt_python,
			status.remotePython,
			lt_size,
			SizeText(status.remoteSize));
	}
	return result;
}

[[nodiscard]] QString RuntimeShortLabel(
		const FreshPlugins::RuntimeStatus &status) {
	using Stage = FreshPlugins::RuntimeStage;
	if (status.stage != Stage::Idle) {
		return RuntimeLabel(status);
	} else if (!status.error.isEmpty()) {
		return tr::fresh_PluginsComponentsError(tr::now);
	} else if (!status.installed) {
		return tr::fresh_PluginsNotInstalled(tr::now);
	} else if (status.updateAvailable) {
		return tr::fresh_PluginsComponentsUpdate(tr::now);
	}
	return tr::fresh_PluginsComponentsInstalled(tr::now);
}

[[nodiscard]] QString ComponentsStatusText(
		const FreshPlugins::RuntimeStatus &status) {
	if (status.stage != FreshPlugins::RuntimeStage::Idle
		|| !status.error.isEmpty()
		|| (status.installed && status.updateAvailable)) {
		return RuntimeLabel(status);
	} else if (!status.installed) {
		return tr::fresh_PluginsNoRuntime(tr::now);
	}
	return status.remoteKnown
		? tr::fresh_PluginsUpToDate(tr::now)
		: tr::fresh_PluginsComponentsInstalled(tr::now);
}

[[nodiscard]] QString VersionText(const QString &installed, const QString &remote) {
	const auto now = installed.isEmpty()
		? tr::fresh_PluginsNotInstalled(tr::now)
		: installed;
	return (remote.isEmpty() || remote == installed)
		? now
		: (now + u" → "_q + remote);
}

void ReportInstall(
		base::weak_ptr<Window::SessionController> weak,
		Fn<void()> ready,
		bool ok) {
	const auto strong = weak.get();
	if (!strong) {
		return;
	} else if (ok) {
		strong->showToast(tr::fresh_PluginsReady(tr::now));
		if (ready) {
			ready();
		}
	} else {
		strong->showToast(tr::fresh_PluginsDownloadFailed(
			tr::now,
			lt_reason,
			FreshPlugins::CurrentRuntimeStatus().error));
	}
}

void OfferDownload(
		not_null<Window::SessionController*> controller,
		Fn<void()> ready) {
	const auto weak = base::make_weak(controller);
	if (FreshPlugins::RuntimePackaged()) {
		FreshPlugins::DownloadRuntime([=](bool ok) {
			ReportInstall(weak, ready, ok);
		});
		return;
	}
	controller->show(Ui::MakeConfirmBox({
		.text = tr::fresh_PluginsDownloadText(tr::now),
		.confirmed = [=](Fn<void()> &&close) {
			close();
			FreshPlugins::DownloadRuntime([=](bool ok) {
				ReportInstall(weak, ready, ok);
			});
		},
		.confirmText = tr::fresh_PluginsDownload(),
	}));
}

void CheckAndOffer(
		not_null<Window::SessionController*> controller,
		Fn<void()> ready) {
	const auto weak = base::make_weak(controller);
	FreshPlugins::CheckRuntime([=](bool ok) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto status = FreshPlugins::CurrentRuntimeStatus();
		if (!ok) {
			strong->showToast(tr::fresh_PluginsDownloadFailed(
				tr::now,
				lt_reason,
				status.error));
		} else if (status.updateAvailable) {
			OfferDownload(strong, ready);
		} else if (status.installed) {
			strong->showToast(tr::fresh_PluginsUpToDate(tr::now));
			if (ready) {
				ready();
			}
		} else {
			strong->showToast(tr::fresh_PluginsDownloadFailed(
				tr::now,
				lt_reason,
				tr::fresh_PluginsUnavailable(tr::now)));
		}
	});
}

void ChooseRuntimeFile(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	FileDialog::GetOpenPath(
		controller->widget().get(),
		tr::fresh_PluginsInstallRuntimeFromFile(tr::now),
		u"runtime.json (*.json)"_q,
		[=](FileDialog::OpenResult &&result) {
			if (result.paths.isEmpty()) {
				return;
			}
			FreshPlugins::InstallRuntimeFromFile(result.paths.front(), [=](bool ok) {
				ReportInstall(weak, [] {
					if (FreshPlugins::SandboxSupported()) {
						AyuSettings::getInstance().setPluginsEnabled(true);
					}
				}, ok);
			});
		});
}

void FillComponentsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(tr::fresh_PluginsComponents());
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();
	const auto version = [&](const QString &name, auto installed, auto remote) {
		const auto row = Settings::AddButtonWithLabel(
			container,
			rpl::single(name),
			FreshPlugins::RuntimeStatusValue(
			) | rpl::map([=](const FreshPlugins::RuntimeStatus &status) {
				return VersionText(status.*installed, status.updateAvailable
					? status.*remote
					: QString());
			}),
			st::settingsButtonNoIcon);
		row->setAttribute(Qt::WA_TransparentForMouseEvents);
	};
	version(
		u"Wasmtime"_q,
		&FreshPlugins::RuntimeStatus::wasmtime,
		&FreshPlugins::RuntimeStatus::remoteWasmtime);
	version(
		u"Python"_q,
		&FreshPlugins::RuntimeStatus::python,
		&FreshPlugins::RuntimeStatus::remotePython);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			FreshPlugins::RuntimeStatusValue() | rpl::map(ComponentsStatusText),
			st::boxDividerLabel),
		st::boxRowPadding + QMargins(0, st::defaultVerticalListSkip, 0, 0));
	Ui::AddSkip(container);

	const auto idle = FreshPlugins::RuntimeStatusValue(
	) | rpl::map([](const FreshPlugins::RuntimeStatus &status) {
		return status.stage == FreshPlugins::RuntimeStage::Idle;
	});
	const auto check = Settings::AddButtonWithIcon(
		container,
		tr::fresh_PluginsCheckUpdates(),
		st::settingsButton,
		{ &st::menuIconDownload });
	rpl::duplicate(idle) | rpl::on_next([=](bool idle) {
		check->setDisabled(!idle);
	}, check->lifetime());
	check->addClickHandler([=] {
		if (FreshPlugins::CurrentRuntimeStatus().stage
			== FreshPlugins::RuntimeStage::Idle) {
			CheckAndOffer(controller, nullptr);
		}
	});
	Settings::AddButtonWithIcon(
		container,
		tr::fresh_PluginsAutoUpdate(),
		st::settingsButton,
		{ &st::menuIconSettings }
	)->toggleOn(
		AyuSettings::getInstance().pluginsRuntimeAutoUpdateValue()
	)->toggledValue(
	) | rpl::on_next([](bool enabled) {
		AyuSettings::getInstance().setPluginsRuntimeAutoUpdate(enabled);
	}, box->lifetime());
	if (!FreshPlugins::RuntimePackaged()) {
		const auto fromFile = Settings::AddButtonWithIcon(
			container,
			tr::fresh_PluginsInstallRuntimeFromFile(),
			st::settingsButton,
			{ &st::menuIconFile });
		rpl::duplicate(idle) | rpl::on_next([=](bool idle) {
			fromFile->setDisabled(!idle);
		}, fromFile->lifetime());
		fromFile->addClickHandler([=] {
			if (FreshPlugins::CurrentRuntimeStatus().stage
				== FreshPlugins::RuntimeStage::Idle) {
				ChooseRuntimeFile(controller);
			}
		});
	}
	const auto removal = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	Settings::AddButtonWithIcon(
		removal->entity(),
		tr::fresh_PluginsRemoveComponents(),
		st::settingsAttentionButtonWithIcon,
		{ &st::menuIconDeleteAttention }
	)->addClickHandler([=] {
		controller->show(Ui::MakeConfirmBox({
			.text = tr::fresh_PluginsRemoveComponentsConfirmation(tr::now),
			.confirmed = [=](Fn<void()> &&close) {
				AyuSettings::getInstance().setPluginsEnabled(false);
				FreshPlugins::RemoveRuntimeFiles();
				close();
			},
			.confirmText = tr::fresh_PluginsRemove(),
			.confirmStyle = &st::attentionBoxButton,
		}));
	});
	removal->toggleOn(
		FreshPlugins::RuntimeStatusValue() | rpl::map([](const auto &status) {
			return status.installed;
		}),
		anim::type::instant);
	Ui::AddSkip(container);
	Ui::AddDividerText(container, tr::fresh_PluginsComponentsAbout());
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace

void AddPluginsSettings(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	const auto toggle = Settings::AddButtonWithIcon(
		container,
		tr::fresh_PluginsEnable(),
		st::settingsButton,
		{ &st::menuIconCustomize });
	toggle->toggleOn(
		AyuSettings::getInstance().pluginsEnabledValue(),
		true);
	toggle->addClickHandler([=] {
		auto &settings = AyuSettings::getInstance();
		const auto busy = (FreshPlugins::CurrentRuntimeStatus().stage
			!= FreshPlugins::RuntimeStage::Idle);
		if (settings.pluginsEnabled()) {
			settings.setPluginsEnabled(false);
		} else if (!FreshPlugins::SandboxSupported()) {
			controller->showToast(tr::fresh_PluginsNoSandbox(tr::now));
		} else if (busy) {
			controller->showToast(RuntimeLabel(FreshPlugins::CurrentRuntimeStatus()));
		} else if (FreshPlugins::CurrentRuntimeStatus().installed) {
			settings.setPluginsEnabled(true);
		} else {
			CheckAndOffer(controller, [] {
				AyuSettings::getInstance().setPluginsEnabled(true);
			});
		}
	});
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		FreshPlugins::StateValue() | rpl::map(StateText));

#ifdef FRESHGRAM_PLUGINS
	const auto ready = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto inner = ready->entity();
	Ui::AddSkip(inner);
	Ui::AddSubsectionTitle(inner, tr::fresh_PluginsInstalled());
	const auto list = inner->add(object_ptr<Ui::VerticalLayout>(inner));
	const auto empty = std::make_shared<rpl::variable<bool>>(true);
	const auto shown = std::make_shared<std::optional<QString>>();
	const auto rows = list->lifetime().make_state<std::vector<PluginRow>>();
	const auto rebuild = [=] {
		const auto snapshot = FreshPlugins::Snapshot();
		const auto plugins = snapshot
			? snapshot->plugins
			: std::vector<fgpl::PluginInfo>();
		auto key = QStringList();
		for (const auto &info : plugins) {
			key.push_back(QString::fromStdString(info.id + ' ' + info.name + ' ' + info.problem));
		}
		if (*shown == key.join('\n')) {
			for (auto i = size_t(0); i != plugins.size() && i != rows->size(); ++i) {
				const auto &info = plugins[i];
				(*rows)[i].status->force_assign(StatusLine(info));
				const auto on = info.enabled && info.problem.empty();
				auto &pending = *(*rows)[i].pending;
				if (pending.since && pending.value != on && crl::now() - pending.since < kPendingToggleMs) {
					continue;
				}
				pending.since = 0;
				if ((*rows)[i].check->checked() != on) {
					(*rows)[i].check->setChecked(on, anim::type::normal);
				}
			}
			return;
		}
		*shown = key.join('\n');
		rows->clear();
		list->clear();
		for (const auto &info : plugins) {
			rows->push_back(AddPluginRow(list, controller, info));
		}
		*empty = plugins.empty();
		RepaintCards(list);
	};
	FreshPlugins::ListChanges(
	) | rpl::on_next([=] {
		Ui::PostponeCall(list, rebuild);
	}, list->lifetime());
	rebuild();
	Settings::AddButtonWithIcon(
		inner,
		tr::fresh_PluginsAdd(),
		st::settingsButtonActive,
		{ &st::menuIconAdd }
	)->addClickHandler([=] {
		if (FreshPlugins::Running()) {
			ChoosePluginFile(controller);
		} else {
			controller->showToast(tr::fresh_PluginsEnableFirst(tr::now));
		}
	});
	Ui::AddSkip(inner);
	Ui::AddDividerText(
		inner,
		empty->value() | rpl::map([](bool empty) {
			return empty
				? tr::fresh_PluginsEmpty(tr::now)
				: tr::fresh_PluginsListAbout(tr::now);
		}));
	ready->toggleOn(
		FreshPlugins::StateValue() | rpl::map([](FreshPlugins::State state) {
			return state == FreshPlugins::State::Ready;
		}),
		anim::type::instant);
#endif

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, tr::fresh_PluginsAdvanced());
	Settings::AddButtonWithLabel(
		container,
		tr::fresh_PluginsComponents(),
		FreshPlugins::RuntimeStatusValue() | rpl::map(RuntimeShortLabel),
		st::settingsButton,
		{ &st::menuIconDownload }
	)->addClickHandler([=] {
		controller->show(Box(FillComponentsBox, controller));
	});
	Settings::AddButtonWithIcon(
		container,
		tr::fresh_PluginsNetwork(),
		st::settingsButton,
		{ &st::menuIconNetwork }
	)->addClickHandler([=] {
		FreshPlugins::ShowNetworkPanel(controller);
	});
	Settings::AddButtonWithIcon(
		container,
		tr::fresh_PluginsNetworkLog(),
		st::settingsButton,
		{ &st::menuIconInfo }
	)->addClickHandler([=] {
		FreshPlugins::ShowNetworkLog(controller);
	});
	Ui::AddSkip(container);
}

} // namespace AyuUi
