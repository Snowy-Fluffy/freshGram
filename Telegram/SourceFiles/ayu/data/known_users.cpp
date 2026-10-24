#include "ayu/data/known_users.h"

#include "ayu/data/ayu_database.h"
#include "ayu/data/messages_storage.h"
#include "ayu/secret/secret_peer.h"
#include "base/unixtime.h"
#include "crl/crl_async.h"
#include "crl/crl_on_main.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_session.h"

#include <map>
#include <memory>

namespace FreshUsers {
namespace {

std::map<std::pair<ID, ID>, QString> Saved;
std::vector<KnownUser> Pending;
bool FlushScheduled = false;

[[nodiscard]] QString SignatureOf(const KnownUser &row) {
	return QString::number(row.accessHash)
		+ '|' + QString::fromStdString(row.firstName)
		+ '|' + QString::fromStdString(row.lastName)
		+ '|' + QString::fromStdString(row.username);
}

void Flush() {
	FlushScheduled = false;
	if (Pending.empty()) {
		return;
	}
	const auto batch = std::make_shared<std::vector<KnownUser>>(
		std::move(Pending));
	Pending.clear();
	crl::async([=] {
		AyuDatabase::saveKnownUsers(*batch);
	});
}

} // namespace

void note(not_null<UserData*> user) {
	if (!user->isLoaded()
		|| user->isInaccessible()
		|| user->isSelf()
		|| user->isServiceUser()
		|| FreshSecret::IsSecretPeer(user)
		|| !user->accessHash()) {
		return;
	}
	auto row = KnownUser();
	row.fakeId = 0;
	row.userId = AyuMessages::storageUserId(user);
	row.peerId = ID(peerToUser(user->id).bare);
	row.accessHash = ID(user->accessHash());
	row.firstName = user->firstName.toStdString();
	row.lastName = user->lastName.toStdString();
	row.username = user->username().toStdString();
	row.updatedAt = base::unixtime::now();

	const auto signature = SignatureOf(row);
	auto &known = Saved[std::make_pair(row.userId, row.peerId)];
	if (known == signature) {
		return;
	}
	known = signature;
	Pending.push_back(std::move(row));
	if (!FlushScheduled) {
		FlushScheduled = true;
		crl::on_main(Flush);
	}
}

UserData *find(not_null<Main::Session*> session, UserId id) {
	auto &owner = session->data();
	if (const auto loaded = owner.userLoaded(id)) {
		return loaded;
	}
	const auto account = ID(session->userId().bare & PeerId::kChatTypeMask);
	const auto row = AyuDatabase::getKnownUser(account, ID(id.bare));
	if (!row) {
		return nullptr;
	}
	const auto user = owner.peer(peerFromUser(id))->asUser();
	if (user && !user->isLoaded()) {
		user->setAccessHash(uint64(row->accessHash));
		user->setName(
			QString::fromStdString(row->firstName),
			QString::fromStdString(row->lastName),
			QString(),
			QString::fromStdString(row->username));
		user->setLoadedStatus(PeerData::LoadedStatus::Normal);
	}
	return user;
}

} // namespace FreshUsers
