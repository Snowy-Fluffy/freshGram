#include "fgpl/permissions.h"

#include <algorithm>

namespace fgpl {
namespace {

const auto kReadingPermissions = std::set<std::string>{
	"messages.read", "messages.read_deleted", "messages.history", "chats.list",
	"chat.info", "account.info", "contacts.list", "composer.draft", "media.read",
	"folders.list", "settings.read", "stars.balance", "users.info", "chats.active",
	"chats.activity", "users.presence", "app.activity", "translate.provide",
};

[[nodiscard]] bool Has(
		const std::set<std::string> &granted,
		const char *permission) {
	return granted.contains(permission);
}

const auto kHighPermissions = std::set<std::string>{
	"messages.send", "messages.transform_outgoing", "messages.read_deleted",
	"net.tcp", "net.udp", "background", "fs.user_selected", "clipboard",
	"messages.edit", "messages.delete", "ghost.control",
};

const auto kLowPermissions = std::set<std::string>{
	"storage.private", "settings.read", "ui.menu", "ui.command",
	"ui.settings_page", "ui.composer_button", "ui.dialog", "ui.toast", "ui.badges",
	"notifications", "messages.ids",
};

} // namespace

Danger PermissionDanger(const std::string &id, const std::string &scope) {
	if (kHighPermissions.contains(id)
		|| (id == "messages.read" && scope == "all")) {
		return Danger::High;
	} else if (kLowPermissions.contains(id)) {
		return Danger::Low;
	}
	return Danger::Medium;
}

Grants MakeGrants(const Manifest &manifest, const GrantDecision &decision) {
	auto grants = Grants();
	grants.pluginId = manifest.id;
	grants.chats = decision.chats;
	grants.storageQuotaBytes = uint64_t(manifest.storageMb) << 20;
	for (const auto &request : manifest.permissions) {
		if (!decision.granted.contains(request.id)) {
			continue;
		} else if (request.scope == "selected" && !decision.chats) {
			continue;
		}
		grants.permissions.insert(request.id);
	}
	return grants;
}

std::vector<RiskNote> RiskNotes(const std::set<std::string> &granted) {
	auto notes = std::vector<RiskNote>();
	const auto reads = std::any_of(
		granted.begin(),
		granted.end(),
		[](const std::string &id) { return kReadingPermissions.contains(id); });
	const auto sockets = Has(granted, "net.tcp") || Has(granted, "net.udp");
	const auto network = Has(granted, "net.https") || sockets;
	if (reads && network) {
		notes.push_back({
			Risk::ExfiltrationPossible,
			"can read your data and send it to the internet",
		});
	}
	if (network) {
		notes.push_back({ Risk::AnyServer, "can contact any public server" });
	}
	if (sockets) {
		notes.push_back({ Risk::RawSockets, "can open its own TCP or UDP connections" });
	}
	if (Has(granted, "messages.read_deleted")) {
		notes.push_back({
			Risk::DeletedMessages,
			"can read deleted messages kept on this device",
		});
	}
	if (Has(granted, "messages.send")) {
		notes.push_back({ Risk::SendsInYourName, "can send messages in your name" });
	}
	if (Has(granted, "messages.transform_outgoing")) {
		notes.push_back({ Risk::ChangesOutgoing, "can change what you send" });
	}
	if (Has(granted, "messages.edit") || Has(granted, "messages.delete")) {
		notes.push_back({ Risk::ChangesMessages, "can edit or delete messages you sent" });
	}
	if (Has(granted, "background")) {
		notes.push_back({ Risk::Background, "keeps running in the background" });
	}
	if (Has(granted, "clipboard")) {
		notes.push_back({ Risk::Clipboard, "can use the clipboard" });
	}
	if (Has(granted, "fs.user_selected")) {
		notes.push_back({ Risk::Files, "can read and save files you pick" });
	}
	if (Has(granted, "plugins.message")) {
		notes.push_back({
			Risk::PluginMessages,
			"can exchange data with other plugins that accept it",
		});
	}
	return notes;
}

std::vector<std::string> NewRequests(
		const Manifest &previous,
		const Manifest &next) {
	auto result = std::vector<std::string>();
	for (const auto &request : next.permissions) {
		const auto old = std::find_if(
			previous.permissions.begin(),
			previous.permissions.end(),
			[&](const PermissionRequest &item) { return item.id == request.id; });
		if (old == previous.permissions.end()) {
			result.push_back(request.id);
			continue;
		}
		if (request.id == "messages.read"
			&& old->scope != "all"
			&& request.scope == "all") {
			result.push_back("messages.read:all");
		}
	}
	return result;
}

} // namespace fgpl
