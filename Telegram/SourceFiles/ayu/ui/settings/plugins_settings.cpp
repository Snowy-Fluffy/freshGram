// This is the source code of freshGram.
#include "ayu/ui/settings/plugins_settings.h"

#include "lang_auto.h"
#include "ayu/ayu_settings.h"
#include "ayu/features/plugins/plugins_runtime.h"
#include "ayu/features/plugins/plugins_service.h"
#include "ayu/features/plugins/plugins_ui.h"
#include "ayu/ui/boxes/plugin_install_box.h"
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

[[nodiscard]] QString StateText(AyuPlugins::State state) {
	switch (state) {
	case AyuPlugins::State::Unavailable:
		return tr::ayu_PluginsUnavailable(tr::now);
	case AyuPlugins::State::NoSandbox:
		return tr::ayu_PluginsNoSandbox(tr::now);
	case AyuPlugins::State::NoRuntime:
		return tr::ayu_PluginsNoRuntime(tr::now);
	case AyuPlugins::State::NoKey:
		return tr::ayu_PluginsNoKey(tr::now);
	case AyuPlugins::State::Disabled:
	case AyuPlugins::State::Ready:
		return tr::ayu_PluginsDescription(tr::now);
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
			return tr::ayu_PluginStatusDisabled(
				tr::now,
				lt_reason,
				QString::fromStdString(info.disabledReason));
		}
		switch (info.status) {
		case fgpl::PluginStatus::Running:
			return tr::ayu_PluginStatusRunning(tr::now);
		case fgpl::PluginStatus::Disabled:
			return tr::ayu_PluginStatusDisabled(tr::now, lt_reason, QString());
		case fgpl::PluginStatus::Stopped:
			break;
		}
		return tr::ayu_PluginStatusStopped(tr::now);
	}();
	return result + u" · "_q + status;
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
		const auto manager = AyuPlugins::Manager();
		if (!manager) {
			return;
		}
		const auto now = ranges::any_of(manager->list(), [&](const auto &item) {
			return item.id == id && item.enabled;
		});
		manager->setEnabled(id, !now);
		AyuPlugins::NotifyListChanged();
	});
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			StatusText(info),
			st::boxDividerLabel),
		st::defaultBoxDividerLabelPadding);

	const auto details = Settings::AddButtonWithIcon(
		container,
		tr::ayu_PluginDetails(),
		st::settingsButtonNoIcon);
	details->addClickHandler([=] {
		AyuPlugins::ShowPluginDetails(controller, QString::fromStdString(id));
	});
	Ui::AddSkip(container);
}

#endif

[[nodiscard]] QString SizeText(qint64 bytes) {
	return QString::number(std::max<qint64>(1, bytes / (1024 * 1024))) + u" MB"_q;
}

[[nodiscard]] QString RuntimeLabel(const AyuPlugins::RuntimeStatus &status) {
	using Stage = AyuPlugins::RuntimeStage;
	switch (status.stage) {
	case Stage::Checking:
		return tr::ayu_PluginsChecking(tr::now);
	case Stage::Downloading:
		return tr::ayu_PluginsDownloading(
			tr::now,
			lt_percent,
			QString::number(int(status.progress * 100)) + '%');
	case Stage::Installing:
		return tr::ayu_PluginsInstalling(tr::now);
	case Stage::Idle:
		break;
	}
	if (!status.error.isEmpty()) {
		return status.error;
	}
	auto result = status.installed
		? tr::ayu_PluginsVersions(
			tr::now,
			lt_wasmtime,
			status.wasmtime,
			lt_python,
			status.python)
		: tr::ayu_PluginsNotInstalled(tr::now);
	if (status.updateAvailable && status.installed) {
		result = tr::ayu_PluginsUpdateAvailable(
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

void OfferDownload(
		not_null<Window::SessionController*> controller,
		Fn<void()> ready) {
	const auto status = AyuPlugins::CurrentRuntimeStatus();
	controller->show(Ui::MakeConfirmBox({
		.text = tr::ayu_PluginsDownloadText(
			tr::now,
			lt_wasmtime,
			status.remoteWasmtime,
			lt_python,
			status.remotePython,
			lt_channel,
			AyuPlugins::RuntimeChannel(),
			lt_size,
			SizeText(status.remoteSize)),
		.confirmed = [=](Fn<void()> &&close) {
			close();
			AyuPlugins::DownloadRuntime([=](bool ok) {
				const auto error = AyuPlugins::CurrentRuntimeStatus().error;
				if (ok) {
					controller->showToast(tr::ayu_PluginsReady(tr::now));
					if (ready) {
						ready();
					}
				} else {
					controller->showToast(tr::ayu_PluginsDownloadFailed(
						tr::now,
						lt_reason,
						error));
				}
			});
		},
		.confirmText = tr::ayu_PluginsDownload(),
	}));
}

void CheckAndOffer(
		not_null<Window::SessionController*> controller,
		Fn<void()> ready) {
	const auto weak = base::make_weak(controller);
	AyuPlugins::CheckRuntime([=](bool ok) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto status = AyuPlugins::CurrentRuntimeStatus();
		if (!ok) {
			strong->showToast(tr::ayu_PluginsDownloadFailed(
				tr::now,
				lt_reason,
				status.error));
		} else if (status.updateAvailable) {
			OfferDownload(strong, ready);
		} else if (status.installed) {
			strong->showToast(tr::ayu_PluginsUpToDate(tr::now));
			if (ready) {
				ready();
			}
		} else {
			strong->showToast(tr::ayu_PluginsDownloadFailed(
				tr::now,
				lt_reason,
				tr::ayu_PluginsUnavailable(tr::now)));
		}
	});
}

} // namespace

void AddPluginsSettings(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	Ui::AddSubsectionTitle(container, tr::ayu_Plugins());

	const auto toggle = Settings::AddButtonWithIcon(
		container,
		tr::ayu_PluginsEnable(),
		st::settingsButtonNoIcon);
	toggle->toggleOn(
		AyuSettings::getInstance().pluginsEnabledValue(),
		true);
	toggle->addClickHandler([=] {
		auto &settings = AyuSettings::getInstance();
		if (settings.pluginsEnabled()) {
			settings.setPluginsEnabled(false);
		} else if (!AyuPlugins::SandboxSupported()) {
			controller->showToast(tr::ayu_PluginsNoSandbox(tr::now));
		} else if (AyuPlugins::CurrentRuntimeStatus().installed) {
			settings.setPluginsEnabled(true);
		} else {
			CheckAndOffer(controller, [] {
				AyuSettings::getInstance().setPluginsEnabled(true);
			});
		}
	});

	const auto versions = Settings::AddButtonWithLabel(
		container,
		tr::ayu_PluginsCheckUpdates(),
		AyuPlugins::RuntimeStatusValue() | rpl::map(RuntimeLabel),
		st::settingsButtonNoIcon);
	versions->addClickHandler([=] {
		if (AyuPlugins::CurrentRuntimeStatus().stage
			== AyuPlugins::RuntimeStage::Idle) {
			CheckAndOffer(controller, nullptr);
		}
	});

	const auto removal = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto removeButton = Settings::AddButtonWithIcon(
		removal->entity(),
		tr::ayu_PluginsRemoveComponents(),
		st::settingsAttentionButton);
	removeButton->addClickHandler([=] {
		controller->show(Ui::MakeConfirmBox({
			.text = tr::ayu_PluginsRemoveComponentsConfirmation(tr::now),
			.confirmed = [=](Fn<void()> &&close) {
				AyuSettings::getInstance().setPluginsEnabled(false);
				AyuPlugins::RemoveRuntimeFiles();
				close();
			},
			.confirmText = tr::ayu_PluginsRemove(),
		}));
	});
	removal->toggleOn(
		AyuPlugins::RuntimeStatusValue() | rpl::map([](const auto &status) {
			return status.installed;
		}),
		anim::type::instant);
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		AyuPlugins::StateValue() | rpl::map(StateText));

#ifdef FRESHGRAM_PLUGINS
	const auto list = container->add(object_ptr<Ui::VerticalLayout>(container));
	const auto rebuild = [=] {
		list->clear();
		const auto manager = AyuPlugins::Manager();
		if (!manager) {
			return;
		}
		const auto plugins = manager->list();
		if (plugins.empty()) {
			Ui::AddSkip(list);
			list->add(
				object_ptr<Ui::FlatLabel>(
					list,
					tr::ayu_PluginsEmpty(tr::now),
					st::boxDividerLabel),
				st::defaultBoxDividerLabelPadding);
			return;
		}
		Ui::AddSubsectionTitle(list, tr::ayu_PluginsInstalled());
		for (const auto &info : plugins) {
			AddPluginRow(list, controller, info);
		}
	};
	AyuPlugins::ListChanges(
	) | rpl::on_next([=] {
		Ui::PostponeCall(list, rebuild);
	}, list->lifetime());
	rebuild();

	const auto install = Settings::AddButtonWithIcon(
		container,
		tr::ayu_PluginsInstallFromFile(),
		st::settingsButtonNoIcon);
	install->addClickHandler([=] {
		if (AyuPlugins::Manager()) {
			ChoosePluginFile(controller);
		} else {
			controller->showToast(tr::ayu_PluginsEnableFirst(tr::now));
		}
	});
#endif
}

} // namespace AyuUi
