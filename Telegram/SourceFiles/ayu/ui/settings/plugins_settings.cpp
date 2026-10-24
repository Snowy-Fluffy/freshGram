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

[[nodiscard]] QString StatusLine(const fgpl::PluginInfo &info) {
	auto parts = QStringList{ QString::fromStdString(info.version) };
	if (!info.problem.empty()) {
		parts.push_back(tr::fresh_PluginBroken(
			tr::now,
			lt_reason,
			QString::fromStdString(info.problem)));
	} else if (!info.disabledReason.empty()) {
		parts.push_back(tr::fresh_PluginStatusDisabled(
			tr::now,
			lt_reason,
			QString::fromStdString(info.disabledReason)));
	} else if (info.status == fgpl::PluginStatus::Running) {
		parts.push_back(tr::fresh_PluginStatusRunning(tr::now));
	} else if (!info.enabled) {
		parts.push_back(tr::fresh_PluginStatusOff(tr::now));
	} else {
		parts.push_back(tr::fresh_PluginStatusStopped(tr::now));
	}
	if (info.outdated) {
		parts.push_back(tr::fresh_PluginOutdatedShort(tr::now));
	}
	return parts.join(u" · "_q);
}

void AddPluginRow(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		const fgpl::PluginInfo &info) {
	const auto id = info.id;
	const auto broken = !info.problem.empty();
	const auto name = info.name.empty()
		? QString::fromStdString(info.id)
		: QString::fromStdString(info.name);
	const auto [button, toggle, checkView] = Settings::SetupSplitToggle(
		container,
		rpl::single(name),
		&st::menuIconBot,
		info.enabled && !broken,
		rpl::single(StatusLine(info)));
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
		FreshPlugins::Post([=](not_null<fgpl::PluginManager*> manager) {
			manager->setEnabled(id, enable);
		});
	});
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
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			FreshPlugins::RuntimeStatusValue() | rpl::map(RuntimeLabel),
			st::boxLabel));
	Ui::AddSkip(container);

	Settings::AddButtonWithIcon(
		container,
		tr::fresh_PluginsCheckUpdates(),
		st::settingsButton,
		{ &st::menuIconDownload }
	)->addClickHandler([=] {
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
		Settings::AddButtonWithIcon(
			container,
			tr::fresh_PluginsInstallRuntimeFromFile(),
			st::settingsButton,
			{ &st::menuIconFile }
		)->addClickHandler([=] {
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
		if (settings.pluginsEnabled()) {
			settings.setPluginsEnabled(false);
		} else if (!FreshPlugins::SandboxSupported()) {
			controller->showToast(tr::fresh_PluginsNoSandbox(tr::now));
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
	const auto rebuild = [=] {
		list->clear();
		const auto snapshot = FreshPlugins::Snapshot();
		const auto plugins = snapshot
			? snapshot->plugins
			: std::vector<fgpl::PluginInfo>();
		for (const auto &info : plugins) {
			AddPluginRow(list, controller, info);
		}
		*empty = plugins.empty();
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
		FreshPlugins::RuntimeStatusValue() | rpl::map(RuntimeLabel),
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
	Ui::AddSkip(container);
}

} // namespace AyuUi
