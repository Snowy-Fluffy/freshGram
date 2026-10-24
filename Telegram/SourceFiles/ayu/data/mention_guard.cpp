#include "ayu/data/mention_guard.h"

#include "base/call_delayed.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "main/main_session.h"

namespace FreshMentions {
namespace {

constexpr auto kAttempts = 20;
constexpr auto kSettled = 2;
constexpr auto kInterval = crl::time(500);

void Check(not_null<Main::Session*> session, FullMsgId id, int attempt) {
	const auto item = session->data().message(id);
	const auto history = session->data().historyLoaded(id.peer);
	if (!item
		|| !history
		|| !item->isDeleted()
		|| !item->isUnreadMention()
		|| item->mainView()) {
		return;
	}
	if (attempt >= kSettled && !history->blocks.empty()) {
		const auto first = history->blocks.front()->messages.front()->data();
		const auto last = history->blocks.back()->messages.back()->data();
		if (item->date() >= first->date() && item->date() <= last->date()) {
			item->markContentsRead(false);
			return;
		}
	}
	if (attempt + 1 < kAttempts) {
		base::call_delayed(kInterval, session, [=] {
			Check(session, id, attempt + 1);
		});
	}
}

} // namespace

void GuardJump(not_null<Data::Thread*> thread, MsgId msgId) {
	if (!msgId) {
		return;
	}
	const auto history = thread->owningHistory();
	Check(&history->session(), FullMsgId(history->peer->id, msgId), 0);
}

} // namespace FreshMentions
