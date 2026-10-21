#include "permissions.h"

#include <algorithm>

namespace fgpl {
namespace {

const auto kReadingPermissions = std::set<std::string>{
	"messages.read", "messages.read_deleted", "messages.history", "chats.list",
	"chat.info", "account.info", "contacts.list", "composer.draft", "media.read",
	"folders.list", "settings.read", "stars.balance",
};

[[nodiscard]] bool Has(
		const std::set<std::string> &granted,
		const char *permission) {
	return granted.contains(permission);
}

} // namespace

Grants MakeGrants(const Manifest &manifest, const GrantDecision &decision) {
	auto grants = Grants();
	grants.pluginId = manifest.id;
	grants.chats = decision.chats;
	grants.storageQuotaBytes = uint64_t(manifest.storageMb) << 20;
	for (const auto &request : manifest.permissions) {
		if (!decision.granted.contains(request.id)) {
			continue;
		}
		grants.permissions.insert(request.id);
		if (request.id == "net.hosts") {
			grants.hosts.insert(
				grants.hosts.end(),
				request.hosts.begin(),
				request.hosts.end());
		}
	}
	return grants;
}

std::vector<RiskNote> RiskNotes(const std::set<std::string> &granted) {
	auto notes = std::vector<RiskNote>();
	const auto reads = std::any_of(
		granted.begin(),
		granted.end(),
		[](const std::string &id) { return kReadingPermissions.contains(id); });
	const auto network = Has(granted, "net.hosts") || Has(granted, "net.any");
	if (reads && network) {
		notes.push_back({
			Risk::ExfiltrationPossible,
			"can read your data and send it to the internet",
		});
	}
	if (Has(granted, "net.any")) {
		notes.push_back({ Risk::AnyServer, "can contact any server" });
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
	if (Has(granted, "background")) {
		notes.push_back({ Risk::Background, "keeps running in the background" });
	}
	if (Has(granted, "clipboard")) {
		notes.push_back({ Risk::Clipboard, "can use the clipboard" });
	}
	if (Has(granted, "fs.user_selected")) {
		notes.push_back({ Risk::Files, "can read files you pick" });
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
		for (const auto &host : request.hosts) {
			if (std::find(old->hosts.begin(), old->hosts.end(), host)
				== old->hosts.end()) {
				result.push_back(request.id + ":" + host);
			}
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
