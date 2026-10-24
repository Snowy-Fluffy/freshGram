#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>

class UserData;

namespace Main {
class Session;
} // namespace Main

namespace FreshIdSearch {

enum class Type {
	None,
	UserOnly,
	ChatOnly,
	Both,
};

struct Query {
	Type type = Type::None;
	qint64 id = 0;

	[[nodiscard]] bool valid() const {
		return type != Type::None;
	}
	[[nodiscard]] bool canBeUser() const {
		return type == Type::UserOnly || type == Type::Both;
	}
	[[nodiscard]] bool canBeChat() const {
		return type == Type::ChatOnly || type == Type::Both;
	}
};

[[nodiscard]] Query Parse(const QString &query);

[[nodiscard]] UserData *FindUser(
	not_null<Main::Session*> session,
	const Query &query);

} // namespace FreshIdSearch
