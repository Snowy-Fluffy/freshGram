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
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
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

[[nodiscard]] QString StatusText(const fgpl::PluginInfo &info) {
	auto result = QString::fromStdString(info.version);
	if (!info.publisher.empty()) {
		result += u" · "_q + QString::fromStdString(info.publisher);
	}
	const auto status = [&] {
		if (!info.disabledReason.empty()) {
			return tr::fresh_PluginStatusDisabled(
				tr::now,
				lt_reason,
				QString::fromStdString(info.disabledReason));
		}
		switch (info.status) {
		case fgpl::PluginStatus::Running:
			return tr::fresh_PluginStatusRunning(tr::now);
		case fgpl::PluginStatus::Disabled:
			return tr::fresh_PluginStatusDisabled(tr::now, lt_reason, QString());
		case fgpl::PluginStatus::Stopped:
			break;
		}
		return tr::fresh_PluginStatusStopped(tr::now);
	}();
	result += u" · "_q + status;
	if (info.outdated) {
		result += u" · "_q + tr::fresh_PluginOutdatedShort(tr::now);
	}
	return result;
}

void AddPluginRow(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		const fgpl::PluginInfo &info) {
	const auto id = info.id;
	const auto name = QString::fromStdString(info.name);
	const auto button = Settings::AddButtonWithIcon(
		container,
		rpl::single(name),
		st::settingsButtonNoIcon);
	button->toggleOn(rpl::single(info.enabled), true);
	button->addClickHandler([=] {
		const auto snapshot = FreshPlugins::Snapshot();
		const auto found = snapshot ? snapshot->plugin(id) : nullptr;
		if (!found) {
			return;
		}
		FreshPlugins::Post([=, enable = !found->enabled](
				not_null<fgpl::PluginManager*> manager) {
			manager->setEnabled(id, enable);
		});
	});
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			StatusText(info),
			st::boxDividerLabel),
		st::defaultBoxDividerLabelPadding);

	const auto details = Settings::AddButtonWithIcon(
		container,
		tr::fresh_PluginDetails(),
		st::settingsButtonNoIcon);
	details->addClickHandler([=] {
		FreshPlugins::ShowPluginDetails(controller, QString::fromStdString(id));
	});
	Ui::AddSkip(container);
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
	const auto status = FreshPlugins::CurrentRuntimeStatus();
	controller->show(Ui::MakeConfirmBox({
		.text = tr::fresh_PluginsDownloadText(
			tr::now,
			lt_wasmtime,
			status.remoteWasmtime,
			lt_python,
			status.remotePython,
			lt_channel,
			FreshPlugins::RuntimeChannel(),
			lt_size,
			SizeText(status.remoteSize)),
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

} // namespace

void AddPluginsSettings(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	const auto toggle = Settings::AddButtonWithIcon(
		container,
		tr::fresh_PluginsEnable(),
		st::settingsButtonNoIcon);
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

	const auto versions = Settings::AddButtonWithLabel(
		container,
		tr::fresh_PluginsCheckUpdates(),
		FreshPlugins::RuntimeStatusValue() | rpl::map(RuntimeLabel),
		st::settingsButtonNoIcon);
	versions->addClickHandler([=] {
		if (FreshPlugins::CurrentRuntimeStatus().stage
			== FreshPlugins::RuntimeStage::Idle) {
			CheckAndOffer(controller, nullptr);
		}
	});

	Settings::AddButtonWithIcon(
		container,
		tr::fresh_PluginsAutoUpdate(),
		st::settingsButtonNoIcon
	)->toggleOn(
		AyuSettings::getInstance().pluginsRuntimeAutoUpdateValue()
	)->toggledValue(
	) | rpl::on_next([](bool enabled) {
		AyuSettings::getInstance().setPluginsRuntimeAutoUpdate(enabled);
	}, container->lifetime());

	Settings::AddButtonWithIcon(
		container,
		tr::fresh_PluginsNetwork(),
		st::settingsButtonNoIcon
	)->addClickHandler([=] {
		FreshPlugins::ShowNetworkPanel(controller);
	});

	if (!FreshPlugins::RuntimePackaged()) {
		const auto offline = Settings::AddButtonWithIcon(
			container,
			tr::fresh_PluginsInstallRuntimeFromFile(),
			st::settingsButtonNoIcon);
		offline->addClickHandler([=] {
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
	const auto removeButton = Settings::AddButtonWithIcon(
		removal->entity(),
		tr::fresh_PluginsRemoveComponents(),
		st::settingsAttentionButton);
	removeButton->addClickHandler([=] {
		controller->show(Ui::MakeConfirmBox({
			.text = tr::fresh_PluginsRemoveComponentsConfirmation(tr::now),
			.confirmed = [=](Fn<void()> &&close) {
				AyuSettings::getInstance().setPluginsEnabled(false);
				FreshPlugins::RemoveRuntimeFiles();
				close();
			},
			.confirmText = tr::fresh_PluginsRemove(),
		}));
	});
	removal->toggleOn(
		FreshPlugins::RuntimeStatusValue() | rpl::map([](const auto &status) {
			return status.installed;
		}),
		anim::type::instant);
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		FreshPlugins::StateValue() | rpl::map(StateText));

#ifdef FRESHGRAM_PLUGINS
	const auto list = container->add(object_ptr<Ui::VerticalLayout>(container));
	const auto rebuild = [=] {
		list->clear();
		const auto snapshot = FreshPlugins::Snapshot();
		if (!snapshot) {
			return;
		}
		const auto &plugins = snapshot->plugins;
		if (plugins.empty()) {
			Ui::AddSkip(list);
			list->add(
				object_ptr<Ui::FlatLabel>(
					list,
					tr::fresh_PluginsEmpty(tr::now),
					st::boxDividerLabel),
				st::defaultBoxDividerLabelPadding);
			return;
		}
		Ui::AddSubsectionTitle(list, tr::fresh_PluginsInstalled());
		for (const auto &info : plugins) {
			AddPluginRow(list, controller, info);
		}
	};
	FreshPlugins::ListChanges(
	) | rpl::on_next([=] {
		Ui::PostponeCall(list, rebuild);
	}, list->lifetime());
	rebuild();

	const auto install = Settings::AddButtonWithIcon(
		container,
		tr::fresh_PluginsInstallFromFile(),
		st::settingsButtonNoIcon);
	install->addClickHandler([=] {
		if (FreshPlugins::Running()) {
			ChoosePluginFile(controller);
		} else {
			controller->showToast(tr::fresh_PluginsEnableFirst(tr::now));
		}
	});
#endif
}

} // namespace AyuUi
