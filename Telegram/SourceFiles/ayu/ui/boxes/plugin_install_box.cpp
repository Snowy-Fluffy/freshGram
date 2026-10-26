// This is the source code of freshGram.
#include "ayu/ui/boxes/plugin_install_box.h"

#include "lang/lang_keys.h"
#include "ayu/features/plugins/plugins_runtime.h"
#include "ayu/features/plugins/plugins_service.h"
#include "ayu/features/plugins/plugins_ui.h"
#include "core/file_utilities.h"
#include "crl/crl_on_main.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "styles/style_ayu_styles.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "core/version.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"

#include <algorithm>

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/manager.h"
#include "fgpl/permissions.h"
#endif

namespace AyuUi {
namespace {

constexpr auto kMaxPackageBytes = qint64(64) << 20;

#ifdef FRESHGRAM_PLUGINS

[[nodiscard]] std::span<const uint8_t> BytesOf(const QByteArray &data) {
	return {
		reinterpret_cast<const uint8_t*>(data.constData()),
		size_t(data.size()),
	};
}

[[nodiscard]] QString FailureReason(const fgpl::InstallFailure &failure) {
	auto result = QString();
	switch (failure.error) {
	case fgpl::InstallError::Package: result = u"damaged package"_q; break;
	case fgpl::InstallError::Incompatible: result = u"incompatible with this app"_q; break;
	case fgpl::InstallError::UnsignedRefused: result = u"unsigned package"_q; break;
	case fgpl::InstallError::PublisherChanged: result = u"publisher changed"_q; break;
	case fgpl::InstallError::SameVersion: result = u"this version is already installed"_q; break;
	case fgpl::InstallError::DowngradeRefused: result = u"a newer version is installed"_q; break;
	case fgpl::InstallError::MissingDependency: result = u"missing dependency"_q; break;
	case fgpl::InstallError::Conflict: result = u"conflicts with an installed plugin"_q; break;
	case fgpl::InstallError::Locked: result = u"the app is locked"_q; break;
	case fgpl::InstallError::Io: result = u"disk error"_q; break;
	}
	if (!failure.details.empty()) {
		result += u" ("_q + QString::fromStdString(failure.details.front()) + ')';
	}
	return result;
}

void Install(
		not_null<Window::SessionController*> controller,
		std::shared_ptr<QByteArray> archive,
		fgpl::GrantDecision decision,
		fgpl::InstallOptions options,
		Fn<void()> done,
		Fn<void()> failed = nullptr);

void Installed(
		not_null<Window::SessionController*> controller,
		std::shared_ptr<QByteArray> archive,
		fgpl::GrantDecision decision,
		fgpl::InstallOptions options,
		Fn<void()> done,
		const fgpl::Result<fgpl::InstallReport, fgpl::InstallFailure> &result) {
	if (!result.ok()) {
		const auto failure = result.error();
		if (failure.error == fgpl::InstallError::PublisherChanged
			&& !options.acceptNewPublisher) {
			controller->show(Ui::MakeConfirmBox({
				.text = tr::fresh_PluginPublisherChanged(tr::now),
				.confirmed = [=](Fn<void()> &&close) {
					auto again = options;
					again.acceptNewPublisher = true;
					close();
					Install(controller, archive, decision, again, done);
				},
				.confirmText = tr::fresh_PluginInstall(),
			}));
			return;
		}
		controller->showToast(tr::fresh_PluginInstallFailed(
			tr::now,
			lt_reason,
			FailureReason(failure)));
		return;
	}
	controller->showToast(tr::fresh_PluginInstalledToast(tr::now));
	if (done) {
		done();
	}
}

void Install(
		not_null<Window::SessionController*> controller,
		std::shared_ptr<QByteArray> archive,
		fgpl::GrantDecision decision,
		fgpl::InstallOptions options,
		Fn<void()> done,
		Fn<void()> failed) {
	if (!FreshPlugins::Running()) {
		controller->showToast(tr::fresh_PluginsEnableFirst(tr::now));
		if (failed) {
			failed();
		}
		return;
	}
	const auto account = FreshPlugins::AccountId(&controller->session())
		.toStdString();
	const auto weak = base::make_weak(controller);
	FreshPlugins::Post([=](not_null<fgpl::PluginManager*> manager) {
		auto result = std::make_shared<
			fgpl::Result<fgpl::InstallReport, fgpl::InstallFailure>>(
				manager->install(BytesOf(*archive), options));
		if (result->ok()) {
			const auto id = result->value().id;
			manager->grant(id, account, decision);
			if (!result->value().updated) {
				manager->setEnabled(id, true);
			}
			manager->startEnabled(account);
		}
		crl::on_main([=] {
			if (!result->ok() && failed) {
				failed();
			}
			if (const auto strong = weak.get()) {
				Installed(strong, archive, decision, options, done, *result);
			}
		});
	});
}

void FillInstallBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		std::shared_ptr<QByteArray> archive,
		std::shared_ptr<fgpl::Package> package) {
	const auto &manifest = package->manifest;
	const auto snapshot = FreshPlugins::Snapshot();
	const auto installed = [&]() -> std::optional<fgpl::PluginInfo> {
		const auto found = snapshot ? snapshot->plugin(manifest.id) : nullptr;
		return found ? std::make_optional(*found) : std::nullopt;
	}();
	const auto installedVersion = installed
		? fgpl::ParseVersion(installed->version)
		: std::nullopt;
	const auto order = installedVersion
		? fgpl::Compare(manifest.version, *installedVersion)
		: 1;
	const auto previous = (installed && snapshot)
		? snapshot->manifest(manifest.id)
		: nullptr;
	const auto granted = (installed && snapshot)
		? snapshot->grantsOf(manifest.id)
		: std::nullopt;
	const auto added = previous
		? fgpl::NewRequests(*previous, manifest)
		: std::vector<std::string>();
	box->setTitle((!installed || order < 0)
		? tr::fresh_PluginInstallTitle()
		: (order > 0)
		? tr::fresh_PluginUpdateTitle()
		: tr::fresh_PluginInstalledTitle());
	box->setWidth(st::boxWideWidth);

	const auto signedPackage = (package->signature == fgpl::SignatureState::Valid);
	const auto official = FreshPlugins::IsOfficialPublisher(package->publisher);
	const auto icon = package->files.find(manifest.icon);
	FreshPlugins::AddPluginHeader(
		box->verticalLayout(),
		FreshPlugins::HeaderFor(
			manifest,
			package->publisher,
			(icon != end(package->files))
				? QByteArray(
					reinterpret_cast<const char*>(icon->second.data()),
					qsizetype(icon->second.size()))
				: QByteArray()));
	Ui::AddSkip(box->verticalLayout());
	if (!official) {
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			tr::fresh_PluginUnofficialWarning(tr::now),
			st::pluginDangerHigh));
	}
	auto environment = fgpl::HostEnvironment();
	if (const auto app = fgpl::ParseVersion(AppVersionStr)) {
		environment.app = *app;
	}
	if (fgpl::Outdated(manifest, environment)) {
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			tr::fresh_PluginOutdated(
				tr::now,
				lt_version,
				QString::fromStdString(manifest.testedWith->text())),
			st::pluginDangerMedium));
	}

	if (installed) {
		const auto from = QString::fromStdString(installed->version);
		const auto to = QString::fromStdString(manifest.version.text());
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			(order > 0)
				? tr::fresh_PluginUpdateFrom(tr::now, lt_from, from, lt_to, to)
				: (order == 0)
				? tr::fresh_PluginSameVersion(tr::now, lt_version, from)
				: tr::fresh_PluginNewerInstalled(tr::now, lt_from, from, lt_to, to),
			(order < 0) ? st::pluginDangerHigh : st::boxDividerLabel));
	}
	if (installed && order == 0) {
		const auto id = QString::fromStdString(manifest.id);
		box->addButton(tr::fresh_PluginDetails(), [=] {
			box->closeBox();
			FreshPlugins::ShowPluginDetails(controller, id);
		});
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		return;
	}

	if (snapshot) {
		const auto language = Lang::Id().toStdString();
		const auto account = FreshPlugins::AccountId(&controller->session()).toStdString();
		for (const auto &other : snapshot->contributions(account, language)) {
			if (other.id == manifest.id) {
				continue;
			}
			for (const auto &command : manifest.commands) {
				const auto taken = std::any_of(
					other.commands.begin(),
					other.commands.end(),
					[&](const fgpl::ContributedCommand &existing) {
						return existing.name == command.name;
					});
				if (taken) {
					box->addRow(object_ptr<Ui::FlatLabel>(
						box,
						tr::fresh_PluginCommandTaken(
							tr::now,
							lt_command,
							u"/"_q + QString::fromStdString(command.name),
							lt_plugin,
							QString::fromStdString(other.name)),
						st::pluginDangerMedium));
				}
			}
		}
	}
	if (!manifest.messagesFrom.empty()) {
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			FreshPlugins::AcceptsFrom(manifest.messagesFrom),
			st::boxDividerLabel));
	}

	auto contributes = QStringList();
	if (!manifest.menus.empty()) {
		contributes.push_back(tr::fresh_PluginContribMenus(tr::now));
	}
	if (!manifest.commands.empty()) {
		contributes.push_back(tr::fresh_PluginContribCommands(tr::now));
	}
	if (manifest.settingsPage) {
		contributes.push_back(tr::fresh_PluginContribSettings(tr::now));
	}
	if (!contributes.isEmpty()) {
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			tr::fresh_PluginContributes(tr::now, lt_list, contributes.join(u", "_q)),
			st::boxDividerLabel));
	}
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		FreshPlugins::LimitsText(manifest),
		st::boxDividerLabel));

	Ui::AddSkip(box->verticalLayout());
	Ui::AddDivider(box->verticalLayout());
	Ui::AddSkip(box->verticalLayout());
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		tr::fresh_PluginPermissions(tr::now),
		st::boxTitle));
	const auto decision = FreshPlugins::AddPermissionsEditor(
		box,
		controller,
		manifest,
		granted,
		added);

	const auto weak = base::make_weak(box);
	const auto downgrade = installed && (order < 0);
	const auto busy = std::make_shared<bool>(false);
	box->addButton(!installed
		? tr::fresh_PluginInstall()
		: downgrade
		? tr::fresh_PluginInstallOlder()
		: tr::fresh_PluginUpdate(), [=] {
		if (*busy) {
			return;
		}
		const auto chosen = decision();
		if (!chosen) {
			return;
		}
		*busy = true;
		auto options = fgpl::InstallOptions();
		options.acceptUnsigned = !signedPackage;
		options.allowDowngrade = downgrade;
		Install(controller, archive, *chosen, options, [=] {
			if (const auto strong = weak.get()) {
				strong->closeBox();
			}
		}, [=] {
			*busy = false;
		});
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

#endif

} // namespace

void ShowPluginInstallBox(
		not_null<Window::SessionController*> controller,
		QByteArray archive) {
#ifdef FRESHGRAM_PLUGINS
	if (!FreshPlugins::Running()) {
		controller->showToast(tr::fresh_PluginsEnableFirst(tr::now));
		return;
	}
	const auto bytes = std::make_shared<QByteArray>(std::move(archive));
	auto opened = fgpl::OpenPackage(BytesOf(*bytes));
	if (!opened.ok()) {
		const auto failure = opened.error();
		controller->showToast(tr::fresh_PluginInstallFailed(
			tr::now,
			lt_reason,
			failure.details.empty()
				? u"damaged package"_q
				: QString::fromStdString(failure.details.front())));
		return;
	}
	const auto package = std::make_shared<fgpl::Package>(
		std::move(opened.value()));
	controller->show(Box(FillInstallBox, controller, bytes, package));
#else
	controller->showToast(tr::fresh_PluginsUnavailable(tr::now));
#endif
}

void ChoosePluginFile(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	FileDialog::GetOpenPath(
		controller->widget().get(),
		tr::fresh_PluginsInstallFromFile(tr::now),
		u"Plugins (*.fgpl)"_q,
		[=](FileDialog::OpenResult &&result) {
			const auto strong = weak.get();
			if (!strong || result.paths.isEmpty()) {
				return;
			}
			auto file = QFile(result.paths.front());
			if (!file.open(QIODevice::ReadOnly)) {
				strong->showToast(tr::fresh_PluginInstallFailed(
					tr::now,
					lt_reason,
					file.errorString()));
				return;
			} else if (file.size() > kMaxPackageBytes) {
				strong->showToast(tr::fresh_PluginInstallFailed(
					tr::now,
					lt_reason,
					tr::fresh_PluginFileTooLarge(tr::now)));
				return;
			}
			ShowPluginInstallBox(strong, file.readAll());
		});
}

} // namespace AyuUi
