// This is the source code of freshGram.
#include "ayu/ui/boxes/plugin_install_box.h"

#include "lang/lang_keys.h"
#include "ayu/features/plugins/plugins_service.h"
#include "ayu/features/plugins/plugins_ui.h"
#include "core/file_utilities.h"
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

[[nodiscard]] QString Pick(const fgpl::Localized &text) {
	return QString::fromStdString(text.get(Lang::Id().toStdString()));
}

void Install(
		not_null<Window::SessionController*> controller,
		std::shared_ptr<QByteArray> archive,
		fgpl::GrantDecision decision,
		fgpl::InstallOptions options,
		Fn<void()> done) {
	const auto manager = AyuPlugins::Manager();
	if (!manager) {
		controller->showToast(tr::ayu_PluginsEnableFirst(tr::now));
		return;
	}
	auto result = manager->install(BytesOf(*archive), options);
	if (!result.ok()) {
		const auto failure = result.error();
		if (failure.error == fgpl::InstallError::PublisherChanged
			&& !options.acceptNewPublisher) {
			controller->show(Ui::MakeConfirmBox({
				.text = tr::ayu_PluginPublisherChanged(tr::now),
				.confirmed = [=](Fn<void()> &&close) {
					auto again = options;
					again.acceptNewPublisher = true;
					close();
					Install(controller, archive, decision, again, done);
				},
				.confirmText = tr::ayu_PluginInstall(),
			}));
			return;
		}
		controller->showToast(tr::ayu_PluginInstallFailed(
			tr::now,
			lt_reason,
			FailureReason(failure)));
		return;
	}
	const auto id = result.value().id;
	const auto account = AyuPlugins::AccountId(&controller->session())
		.toStdString();
	manager->grant(id, account, decision);
	manager->setEnabled(id, true);
	manager->startEnabled(account);
	AyuPlugins::NotifyListChanged();
	controller->showToast(tr::ayu_PluginInstalledToast(tr::now));
	if (done) {
		done();
	}
}

void FillInstallBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		std::shared_ptr<QByteArray> archive,
		std::shared_ptr<fgpl::Package> package) {
	const auto &manifest = package->manifest;
	const auto manager = AyuPlugins::Manager();
	const auto account = AyuPlugins::AccountId(&controller->session()).toStdString();
	const auto installed = [&]() -> std::optional<fgpl::PluginInfo> {
		for (auto &info : manager ? manager->list() : std::vector<fgpl::PluginInfo>()) {
			if (info.id == manifest.id) {
				return info;
			}
		}
		return std::nullopt;
	}();
	const auto installedVersion = installed
		? fgpl::ParseVersion(installed->version)
		: std::nullopt;
	const auto order = installedVersion
		? fgpl::Compare(manifest.version, *installedVersion)
		: 1;
	const auto previous = (installed && manager)
		? manager->manifest(manifest.id)
		: nullptr;
	const auto granted = (installed && manager)
		? manager->grants(manifest.id, account)
		: std::nullopt;
	const auto added = previous
		? fgpl::NewRequests(*previous, manifest)
		: std::vector<std::string>();
	box->setTitle((!installed || order < 0)
		? tr::ayu_PluginInstallTitle()
		: (order > 0)
		? tr::ayu_PluginUpdateTitle()
		: tr::ayu_PluginInstalledTitle());
	box->setWidth(st::boxWideWidth);

	const auto name = Pick(manifest.name);
	const auto version = QString::fromStdString(manifest.version.text());
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		name + ' ' + version,
		st::boxTitle));
	if (const auto description = Pick(manifest.description); !description.isEmpty()) {
		box->addRow(object_ptr<Ui::FlatLabel>(box, description, st::boxLabel));
	}
	const auto signedPackage = (package->signature == fgpl::SignatureState::Valid);
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		signedPackage
			? tr::ayu_PluginSignedBy(
				tr::now,
				lt_publisher,
				QString::fromStdString(package->publisher))
			: tr::ayu_PluginUnsigned(tr::now),
		st::boxLabel));
	auto environment = fgpl::HostEnvironment();
	if (const auto app = fgpl::ParseVersion(AppVersionStr)) {
		environment.app = *app;
	}
	if (fgpl::Outdated(manifest, environment)) {
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			tr::ayu_PluginOutdated(
				tr::now,
				lt_version,
				QString::fromStdString(manifest.testedWith->text())),
			st::boxDividerLabel));
	}

	if (installed) {
		const auto from = QString::fromStdString(installed->version);
		const auto to = QString::fromStdString(manifest.version.text());
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			(order > 0)
				? tr::ayu_PluginUpdateFrom(tr::now, lt_from, from, lt_to, to)
				: (order == 0)
				? tr::ayu_PluginSameVersion(tr::now, lt_version, from)
				: tr::ayu_PluginNewerInstalled(tr::now, lt_from, from, lt_to, to),
			(order < 0) ? st::pluginDangerHigh : st::boxDividerLabel));
	}
	if (installed && order == 0) {
		const auto id = QString::fromStdString(manifest.id);
		box->addButton(tr::ayu_PluginDetails(), [=] {
			box->closeBox();
			AyuPlugins::ShowPluginDetails(controller, id);
		});
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		return;
	}

	if (!manifest.messagesFrom.empty()) {
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			AyuPlugins::AcceptsFrom(manifest.messagesFrom),
			st::boxDividerLabel));
	}

	auto contributes = QStringList();
	if (!manifest.menus.empty()) {
		contributes.push_back(tr::ayu_PluginContribMenus(tr::now));
	}
	if (!manifest.commands.empty()) {
		contributes.push_back(tr::ayu_PluginContribCommands(tr::now));
	}
	if (manifest.settingsPage) {
		contributes.push_back(tr::ayu_PluginContribSettings(tr::now));
	}
	if (!contributes.isEmpty()) {
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			tr::ayu_PluginContributes(tr::now, lt_list, contributes.join(u", "_q)),
			st::boxDividerLabel));
	}

	auto requested = std::set<std::string>();
	auto boxes = std::make_shared<std::vector<std::pair<std::string, Ui::Checkbox*>>>();
	if (!manifest.permissions.empty()) {
		Ui::AddSubsectionTitle(box->verticalLayout(), tr::ayu_PluginPermissions());
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			tr::ayu_PluginPermissionsAllAccounts(tr::now),
			st::boxDividerLabel));
	}
	for (const auto &request : manifest.permissions) {
		requested.insert(request.id);
		auto label = AyuPlugins::PermissionLabel(request.id);
		if (request.required) {
			label = tr::ayu_PluginPermissionRequired(
				tr::now,
				lt_permission,
				label);
		}
		const auto fresh = std::find(added.begin(), added.end(), request.id) != added.end();
		if (fresh) {
			label = tr::ayu_PluginPermissionNew(tr::now, lt_permission, label);
		}
		const auto checked = request.required
			|| !granted
			|| fresh
			|| granted->granted.contains(request.id);
		const auto check = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			label,
			checked));
		if (request.required) {
			check->setDisabled(true);
		}
		boxes->emplace_back(request.id, check);
		AyuPlugins::AddPermissionDanger(
			box->verticalLayout(),
			request.id,
			request.scope,
			Pick(request.reason));
	}
	for (const auto &note : fgpl::RiskNotes(requested)) {
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			QString::fromStdString(note.text),
			st::boxDividerLabel));
	}

	const auto weak = base::make_weak(box);
	const auto downgrade = installed && (order < 0);
	box->addButton(!installed
		? tr::ayu_PluginInstall()
		: downgrade
		? tr::ayu_PluginInstallOlder()
		: tr::ayu_PluginUpdate(), [=] {
		auto decision = fgpl::GrantDecision();
		for (const auto &[id, check] : *boxes) {
			if (check->checked()) {
				decision.granted.insert(id);
			}
		}
		auto options = fgpl::InstallOptions();
		options.acceptUnsigned = !signedPackage;
		options.allowDowngrade = downgrade;
		Install(controller, archive, std::move(decision), options, [=] {
			if (const auto strong = weak.get()) {
				strong->closeBox();
			}
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
	if (!AyuPlugins::Manager()) {
		controller->showToast(tr::ayu_PluginsEnableFirst(tr::now));
		return;
	}
	const auto bytes = std::make_shared<QByteArray>(std::move(archive));
	auto opened = fgpl::OpenPackage(BytesOf(*bytes));
	if (!opened.ok()) {
		const auto failure = opened.error();
		controller->showToast(tr::ayu_PluginInstallFailed(
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
	controller->showToast(tr::ayu_PluginsUnavailable(tr::now));
#endif
}

void ChoosePluginFile(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	FileDialog::GetOpenPath(
		controller->widget().get(),
		tr::ayu_PluginsInstallFromFile(tr::now),
		u"Plugins (*.fgpl)"_q,
		[=](FileDialog::OpenResult &&result) {
			const auto strong = weak.get();
			if (!strong || result.paths.isEmpty()) {
				return;
			}
			auto file = QFile(result.paths.front());
			if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxPackageBytes) {
				strong->showToast(tr::ayu_PluginInstallFailed(
					tr::now,
					lt_reason,
					file.errorString()));
				return;
			}
			ShowPluginInstallBox(strong, file.readAll());
		});
}

} // namespace AyuUi
