/*
This file is part of freshGram, based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ayu/features/deleted_comments/deleted_comments.h"

#include "apiwrap.h"
#include "data/data_channel.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"

namespace FreshComments {
namespace {

// The automatic forward lands in the discussion group right after the post.
constexpr auto kNewerMessages = 40;
constexpr auto kLookupLimit = 50;

base::flat_set<FullMsgId> Requesting;

[[nodiscard]] MsgId ServerPostId(not_null<HistoryItem*> item) {
	if (!item->isLocal()) {
		return item->id;
	}
	return item->history()->freshOriginalId(item);
}

[[nodiscard]] bool IsForwardOfPost(
		const MTPMessage &message,
		PeerId channelId,
		MsgId postId) {
	return message.match([&](const MTPDmessage &data) {
		const auto fwd = data.vfwd_from();
		if (!fwd) {
			return false;
		}
		const auto &header = fwd->data();
		const auto saved = header.vsaved_from_peer();
		if (saved
			&& peerFromMTP(*saved) == channelId
			&& header.vsaved_from_msg_id().value_or_empty() == postId.bare) {
			return true;
		}
		const auto from = header.vfrom_id();
		return from
			&& peerFromMTP(*from) == channelId
			&& header.vchannel_post().value_or_empty() == postId.bare;
	}, [](const auto &) {
		return false;
	});
}

[[nodiscard]] MsgId FindForwardOfPost(
		not_null<Data::Session*> owner,
		const MTPmessages_Messages &result,
		PeerId channelId,
		MsgId postId) {
	return result.match([](const MTPDmessages_messagesNotModified &) {
		return MsgId();
	}, [&](const auto &data) {
		owner->processUsers(data.vusers());
		owner->processChats(data.vchats());
		for (const auto &message : data.vmessages().v) {
			if (IsForwardOfPost(message, channelId, postId)) {
				return IdFromMessage(message);
			}
		}
		return MsgId();
	});
}

void ShowUnavailable(not_null<Window::SessionController*> controller) {
	controller->showToast(tr::fresh_DeletedPostCommentsUnavailable(tr::now));
}

} // namespace

bool NeedsDiscussionLookup(not_null<HistoryItem*> item) {
	return item->isDeleted() || item->isLocal();
}

void ShowForDeletedPost(
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	const auto history = item->history();
	auto &owner = history->owner();
	const auto comments = item->commentsItemId();
	if (comments.peer && comments.msg) {
		controller->showRepliesForMessage(
			owner.history(comments.peer),
			comments.msg);
		return;
	}
	const auto views = item->Get<HistoryMessageViews>();
	const auto group = (views && views->commentsMegagroupId)
		? owner.channelLoaded(views->commentsMegagroupId)
		: nullptr;
	const auto postId = ServerPostId(item);
	if (!group || !postId) {
		ShowUnavailable(controller);
		return;
	}
	const auto fullId = item->fullId();
	if (!Requesting.emplace(fullId).second) {
		return;
	}
	const auto channelId = history->peer->id;
	const auto groupId = group->id;
	const auto session = &controller->session();
	const auto weak = base::make_weak(controller);
	session->api().request(MTPmessages_GetHistory(
		group->input(),
		MTP_int(0), // offset_id
		MTP_int(item->date()),
		MTP_int(-kNewerMessages),
		MTP_int(kLookupLimit),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		Requesting.remove(fullId);
		const auto rootId = FindForwardOfPost(
			&session->data(),
			result,
			channelId,
			postId);
		const auto strong = weak.get();
		if (!strong) {
			return;
		} else if (!rootId) {
			ShowUnavailable(strong);
			return;
		}
		if (const auto post = session->data().message(fullId)) {
			post->setCommentsItemId(FullMsgId(groupId, rootId));
		}
		strong->showRepliesForMessage(
			session->data().history(groupId),
			rootId);
	}).fail([=](const MTP::Error &error) {
		Requesting.remove(fullId);
		const auto strong = weak.get();
		if (!strong) {
			return;
		} else if (error.type() == u"CHANNEL_PRIVATE"_q
			|| error.type() == u"USER_BANNED_IN_CHANNEL"_q) {
			strong->showToast(tr::lng_group_not_accessible(tr::now));
		} else {
			ShowUnavailable(strong);
		}
	}).send();
}

} // namespace FreshComments
