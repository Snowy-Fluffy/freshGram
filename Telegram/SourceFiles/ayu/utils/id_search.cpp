#include "ayu/utils/id_search.h"

#include "ayu/data/known_users.h"
#include "data/data_user.h"

namespace FreshIdSearch {
namespace {

[[nodiscard]] bool IsNumeric(const QString &str) {
	if (str.isEmpty()) {
		return false;
	}
	for (const auto &ch : str) {
		if (!ch.isDigit()) {
			return false;
		}
	}
	return true;
}

} // namespace

Query Parse(const QString &query) {
	if (query.startsWith(u"id:"_q, Qt::CaseInsensitive)
		|| query.startsWith(u"id "_q, Qt::CaseInsensitive)) {
		const auto idPart = query.mid(3).trimmed();
		if (idPart.startsWith(u"-100"_q)) {
			const auto chatId = idPart.mid(4);
			if (chatId.length() >= 1 && IsNumeric(chatId)) {
				return { Type::ChatOnly, chatId.toLongLong() };
			}
			return {};
		}
		if (idPart.length() >= 5 && IsNumeric(idPart)) {
			return { Type::Both, idPart.toLongLong() };
		}
		return {};
	}

	if (query.startsWith(u"-100"_q)) {
		const auto idPart = query.mid(4);
		if (idPart.length() >= 1 && IsNumeric(idPart)) {
			return { Type::ChatOnly, idPart.toLongLong() };
		}
		return {};
	}

	if (query.length() >= 5 && IsNumeric(query)) {
		return { Type::UserOnly, query.toLongLong() };
	}

	return {};
}

UserData *FindUser(
		not_null<Main::Session*> session,
		const Query &query) {
	if (!query.canBeUser() || query.id <= 0) {
		return nullptr;
	}
	return FreshUsers::find(session, UserId(query.id));
}

} // namespace FreshIdSearch
