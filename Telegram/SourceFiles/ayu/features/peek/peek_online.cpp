#include "ayu/features/peek/peek_online.h"

#include "api/api_user_privacy.h"
#include "apiwrap.h"
#include "ayu/data/ayu_database.h"
#include "base/call_delayed.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "data/data_changes.h"
#include "data/data_lastseen_status.h"
#include "data/data_peer.h"
#include "data/data_peer_values.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "rpl/event_stream.h"
#include "ui/toast/toast.h"

#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>

#include <algorithm>
#include <map>

namespace FreshPeek {
namespace {

using Privacy = Api::UserPrivacy;

constexpr auto kKey = Privacy::Key::LastSeen;
constexpr auto kAttempts = 4;
constexpr auto kRetryDelay = crl::time(700);
constexpr auto kRestoreAttempts = 5;

constexpr auto kAlwaysPremiums = 1;
constexpr auto kAlwaysMiniapps = 2;
constexpr auto kNeverPremiums = 4;
constexpr auto kNeverMiniapps = 8;
constexpr auto kIgnoreAlways = 16;
constexpr auto kIgnoreNever = 32;

struct Update {
	not_null<Main::Session*> session;
	uint64 target = 0;
	std::optional<Result> result;
};

using CacheKey = std::pair<uint64, uint64>;

rpl::event_stream<Update> Updates;
bool Running = false;

[[nodiscard]] std::map<CacheKey, std::optional<Result>> &Cache() {
	static auto result = std::map<CacheKey, std::optional<Result>>();
	return result;
}

void Toast(const QString &text) {
	Ui::Toast::Show(text);
}

[[nodiscard]] bool HasRealPremium(not_null<Main::Session*> session) {
	return (session->user()->flags() & UserDataFlag::Premium) != 0;
}

[[nodiscard]] QString ExactText(const Result &result, TimeId now) {
	const auto seen = (result.kind == Kind::Online && result.checkedAt)
		? result.checkedAt
		: result.time;
	const auto when = base::unixtime::parse(seen);
	const auto today = base::unixtime::parse(now);
	const auto locale = QLocale();
	auto pattern = locale.timeFormat(QLocale::ShortFormat);
	const auto seconds = QRegularExpression(
		u"(?<![A-Za-z])s{1,2}(?![A-Za-z])"_q);
	if (!pattern.contains(seconds)) {
		const auto minutes = QRegularExpression(
			u"(?<![A-Za-z])m{1,2}(?![A-Za-z])"_q).match(pattern);
		if (minutes.hasMatch()) {
			pattern.insert(minutes.capturedEnd(), u":ss"_q);
		}
	}
	const auto time = locale.toString(when.time(), pattern);
	if (when.date() == today.date()) {
		return tr::lng_status_lastseen_today(tr::now, lt_time, time);
	} else if (when.date().addDays(1) == today.date()) {
		return tr::lng_status_lastseen_yesterday(tr::now, lt_time, time);
	}
	return tr::lng_status_lastseen_date_time(
		tr::now,
		lt_date,
		locale.toString(when.date(), QLocale::ShortFormat),
		lt_time,
		time);
}

[[nodiscard]] ID UserKey(not_null<Main::Session*> session) {
	return ID(session->userId().bare & PeerId::kChatTypeMask);
}

[[nodiscard]] std::string Join(
		const std::vector<not_null<PeerData*>> &peers) {
	auto result = QStringList();
	for (const auto &peer : peers) {
		result.push_back(QString::number(peer->id.value));
	}
	return result.join(',').toStdString();
}

[[nodiscard]] std::vector<not_null<PeerData*>> Split(
		not_null<Main::Session*> session,
		const std::string &text) {
	auto result = std::vector<not_null<PeerData*>>();
	const auto parts = QString::fromStdString(text).split(
		',',
		Qt::SkipEmptyParts);
	for (const auto &part : parts) {
		auto ok = false;
		const auto value = part.toULongLong(&ok);
		if (ok && value) {
			result.push_back(session->data().peer(PeerId(value)));
		}
	}
	return result;
}

[[nodiscard]] PeekRestoreRow ToRow(
		not_null<Main::Session*> session,
		const Privacy::Rule &rule) {
	auto row = PeekRestoreRow();
	row.fakeId = 0;
	row.userId = UserKey(session);
	row.option = int(rule.option);
	row.flags = (rule.always.premiums ? kAlwaysPremiums : 0)
		| (rule.always.miniapps ? kAlwaysMiniapps : 0)
		| (rule.never.premiums ? kNeverPremiums : 0)
		| (rule.never.miniapps ? kNeverMiniapps : 0)
		| (rule.ignoreAlways ? kIgnoreAlways : 0)
		| (rule.ignoreNever ? kIgnoreNever : 0);
	row.always = Join(rule.always.peers);
	row.never = Join(rule.never.peers);
	return row;
}

[[nodiscard]] Privacy::Rule FromRow(
		not_null<Main::Session*> session,
		const PeekRestoreRow &row) {
	auto rule = Privacy::Rule();
	rule.option = Privacy::Option(row.option);
	rule.always.premiums = (row.flags & kAlwaysPremiums) != 0;
	rule.always.miniapps = (row.flags & kAlwaysMiniapps) != 0;
	rule.never.premiums = (row.flags & kNeverPremiums) != 0;
	rule.never.miniapps = (row.flags & kNeverMiniapps) != 0;
	rule.ignoreAlways = (row.flags & kIgnoreAlways) != 0;
	rule.ignoreNever = (row.flags & kIgnoreNever) != 0;
	rule.always.peers = Split(session, row.always);
	rule.never.peers = Split(session, row.never);
	return rule;
}

[[nodiscard]] std::optional<Result> Load(not_null<UserData*> user) {
	const auto rows = AyuDatabase::getPeekedStatus(
		UserKey(&user->session()),
		ID(user->id.value & PeerId::kChatTypeMask));
	if (rows.empty()) {
		return std::nullopt;
	}
	const auto kind = rows.front().kind;
	if (kind != int(Kind::Online) && kind != int(Kind::Offline)) {
		return std::nullopt;
	}
	auto result = Result();
	result.kind = Kind(kind);
	result.time = rows.front().time;
	result.checkedAt = rows.front().checkedAt;
	return result;
}

[[nodiscard]] std::optional<Result> Cached(not_null<UserData*> user) {
	const auto key = CacheKey(
		uint64(UserKey(&user->session())),
		uint64(user->id.value & PeerId::kChatTypeMask));
	auto &cache = Cache();
	const auto i = cache.find(key);
	if (i != cache.end()) {
		return i->second;
	}
	return cache.emplace(key, Load(user)).first->second;
}

void Store(not_null<UserData*> user, const Result &result) {
	auto row = PeekedStatusRow();
	row.fakeId = 0;
	row.userId = UserKey(&user->session());
	row.targetId = ID(user->id.value & PeerId::kChatTypeMask);
	row.kind = int(result.kind);
	row.time = result.time;
	row.checkedAt = result.checkedAt;
	AyuDatabase::savePeekedStatus(row);
	Cache()[CacheKey(uint64(row.userId), uint64(row.targetId))] = result;
	Updates.fire({
		.session = &user->session(),
		.target = user->id.value & PeerId::kChatTypeMask,
		.result = result,
	});
	user->session().changes().peerUpdated(
		user,
		Data::PeerUpdate::Flag::OnlineStatus);
}

void Restore(
		not_null<Main::Session*> session,
		Privacy::Rule rule,
		int attempt,
		Fn<void()> finished) {
	session->api().userPrivacy().save(kKey, rule, [=] {
		AyuDatabase::clearPeekRestore(UserKey(session));
		finished();
	}, [=] {
		if (attempt + 1 >= kRestoreAttempts) {
			Toast(tr::fresh_PeekRestoreFailed(tr::now));
			finished();
			return;
		}
		base::call_delayed(kRetryDelay * 3, session, [=] {
			Restore(session, rule, attempt + 1, finished);
		});
	});
}

[[nodiscard]] std::optional<Result> ParseStatus(const MTPUserStatus &status) {
	auto result = Result();
	result.checkedAt = base::unixtime::now();
	return status.match([&](const MTPDuserStatusOnline &data)
	-> std::optional<Result> {
		result.kind = Kind::Online;
		result.time = data.vexpires().v;
		return result;
	}, [&](const MTPDuserStatusOffline &data) -> std::optional<Result> {
		result.kind = Kind::Offline;
		result.time = data.vwas_online().v;
		return result;
	}, [](const auto &) -> std::optional<Result> {
		return std::nullopt;
	});
}

void Fetch(
		not_null<UserData*> user,
		int attempt,
		Fn<void(std::optional<Result>)> done) {
	const auto session = &user->session();
	const auto userId = peerToUser(user->id);
	QVector<MTPInputUser> input;
	input.push_back(user->inputUser());
	session->api().request(MTPusers_GetUsers(
		MTP_vector<MTPInputUser>(input)
	)).done([=](const MTPVector<MTPUser> &result) {
		session->data().processUsers(result);
		auto found = std::optional<Result>();
		const auto bare = user->id.value & PeerId::kChatTypeMask;
		for (const auto &item : result.v) {
			item.match([&](const MTPDuser &data) {
				if (uint64(data.vid().v) != bare) {
					return;
				}
				if (const auto status = data.vstatus()) {
					found = ParseStatus(*status);
				}
			}, [](const auto &) {
			});
		}
		if (found || attempt + 1 >= kAttempts) {
			done(found);
			return;
		}
		base::call_delayed(kRetryDelay, session, [=] {
			if (const auto strong = session->data().userLoaded(userId)) {
				Fetch(strong, attempt + 1, done);
			} else {
				done(std::nullopt);
			}
		});
	}).fail([=] {
		done(std::nullopt);
	}).send();
}

void Begin(not_null<UserData*> user, const Privacy::Rule &original) {
	const auto session = &user->session();
	const auto userId = peerToUser(user->id);
	const auto finish = [=](std::optional<Result> result, bool changed) {
		const auto finalize = [=] {
			Running = false;
			if (const auto strong = session->data().userLoaded(userId)) {
				if (result) {
					Store(strong, *result);
					Toast(format(*result));
				} else {
					Toast(tr::fresh_PeekFailed(tr::now));
				}
			}
		};
		if (changed) {
			Restore(session, original, 0, finalize);
		} else {
			finalize();
		}
	};

	auto modified = original;
	modified.ignoreAlways = false;
	const auto target = not_null<PeerData*>(user);
	auto &never = modified.never.peers;
	never.erase(
		std::remove(never.begin(), never.end(), target),
		never.end());
	auto &always = modified.always.peers;
	if (std::find(always.begin(), always.end(), target) == always.end()) {
		always.push_back(target);
	}
	const auto already = (original.option == Privacy::Option::Everyone
		&& std::find(
			original.never.peers.begin(),
			original.never.peers.end(),
			target) == original.never.peers.end());
	if (already) {
		Fetch(user, 0, [=](std::optional<Result> result) {
			finish(result, false);
		});
		return;
	}
	AyuDatabase::savePeekRestore(ToRow(session, original));
	Toast(tr::fresh_PeekChecking(tr::now));
	session->api().userPrivacy().save(kKey, modified, [=] {
		Fetch(user, 0, [=](std::optional<Result> result) {
			finish(result, true);
		});
	}, [=] {
		AyuDatabase::clearPeekRestore(UserKey(session));
		finish(std::nullopt, false);
	});
}

} // namespace

bool available(not_null<UserData*> user) {
	return !user->isSelf()
		&& !user->isBot()
		&& !user->isInaccessible()
		&& !user->isServiceUser()
		&& !HasRealPremium(&user->session());
}

bool shouldOffer(PeerData *peer) {
	const auto user = peer ? peer->asUser() : nullptr;
	if (!user || !available(user)) {
		return false;
	}
	const auto &lastseen = user->lastseen();
	return lastseen.isHidden() && !lastseen.isLocalOnlineValue();
}

void start(not_null<UserData*> user) {
	const auto session = &user->session();
	if (HasRealPremium(session)) {
		Toast(tr::fresh_PeekPremium(tr::now));
		return;
	} else if (Running) {
		Toast(tr::fresh_PeekBusy(tr::now));
		return;
	}
	Running = true;
	auto &privacy = session->api().userPrivacy();
	privacy.reload(kKey);
	const auto userId = peerToUser(user->id);
	const auto began = std::make_shared<bool>(false);
	privacy.value(
		kKey
	) | rpl::take(1) | rpl::on_next([=](const Privacy::Rule &rule) {
		*began = true;
		if (const auto strong = session->data().userLoaded(userId)) {
			Begin(strong, rule);
		} else {
			Running = false;
		}
	}, session->lifetime());
	base::call_delayed(kRetryDelay * 15, session, [=] {
		if (!*began) {
			Running = false;
			Toast(tr::fresh_PeekFailed(tr::now));
		}
	});
}

void restoreIfNeeded(not_null<Main::Session*> session) {
	if (Running) {
		return;
	}
	const auto rows = AyuDatabase::getPeekRestore(UserKey(session));
	if (rows.empty()) {
		return;
	}
	Running = true;
	Restore(session, FromRow(session, rows.front()), 0, [] {
		Running = false;
	});
}

rpl::producer<std::optional<Result>> value(not_null<UserData*> user) {
	const auto target = user->id.value & PeerId::kChatTypeMask;
	const auto session = &user->session();
	return rpl::single(
		Cached(user)
	) | rpl::then(
		Updates.events(
		) | rpl::filter([=](const Update &update) {
			return (update.session == session) && (update.target == target);
		}) | rpl::map([](const Update &update) {
			return update.result;
		})
	);
}

QString format(const Result &result, bool) {
	return ExactText(result, base::unixtime::now());
}

QString augment(
		not_null<UserData*> user,
		const QString &telegramText,
		TimeId now,
		bool full) {
	if (user->isSelf()
		|| user->isBot()
		|| user->isServiceUser()
		|| HasRealPremium(&user->session())) {
		return telegramText;
	}
	const auto &lastseen = user->lastseen();
	if (!lastseen.isHidden() || lastseen.isLocalOnlineValue()) {
		return telegramText;
	}
	const auto result = Cached(user);
	if (!result) {
		return telegramText;
	}
	return tr::fresh_PeekStatusFormat(
		tr::now,
		lt_status,
		telegramText,
		lt_peeked,
		ExactText(*result, now));
}

} // namespace FreshPeek
