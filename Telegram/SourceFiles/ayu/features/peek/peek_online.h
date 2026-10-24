#pragma once

#include "base/basic_types.h"
#include "rpl/producer.h"

#include <optional>

class PeerData;
class UserData;

namespace Main {
class Session;
} // namespace Main

namespace FreshPeek {

enum class Kind : int {
	Online = 0,
	Offline = 1,
};

struct Result {
	Kind kind = Kind::Offline;
	int time = 0;
	int checkedAt = 0;
};

[[nodiscard]] bool available(not_null<UserData*> user);
[[nodiscard]] bool shouldOffer(PeerData *peer);

void start(not_null<UserData*> user);
void restoreIfNeeded(not_null<Main::Session*> session);

[[nodiscard]] rpl::producer<std::optional<Result>> value(
	not_null<UserData*> user);
[[nodiscard]] QString format(const Result &result, bool full = true);
[[nodiscard]] QString augment(
	not_null<UserData*> user,
	const QString &telegramText,
	TimeId now,
	bool full = false);

} // namespace FreshPeek
