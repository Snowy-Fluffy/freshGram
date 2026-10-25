/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "dialogs/dialogs_search_from_controllers.h"

#include "lang/lang_keys.h"
#include "data/data_peer_values.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "main/main_session.h"
#include "main/session/send_as_peers.h"
#include "apiwrap.h"

namespace Dialogs {

object_ptr<Ui::BoxContent> SearchFromBox(
		not_null<PeerData*> peer,
		Fn<void(not_null<PeerData*>)> callback,
		Fn<void()> closedCallback) {
	auto createController = [
		peer,
		callback = std::move(callback)
	]() -> std::unique_ptr<PeerListController> {
		if (peer && (peer->isChat() || peer->isMegagroup())) {
			return std::make_unique<Dialogs::SearchFromController>(
				peer,
				std::move(callback));
		}
		return nullptr;
	};
	if (auto controller = createController()) {
		auto subscription = std::make_shared<rpl::lifetime>();
		auto box = Box<PeerListBox>(
			std::move(controller),
			[subscription](not_null<PeerListBox*> box) {
				box->addButton(tr::lng_cancel(), [box, subscription] {
					box->closeBox();
				});
			});
		box->boxClosing() | rpl::on_next(
			std::move(closedCallback),
			*subscription);
		return box;
	}
	return nullptr;
}

SearchFromController::SearchFromController(
	not_null<PeerData*> peer,
	Fn<void(not_null<PeerData*>)> callback)
: AddSpecialBoxController(
	peer,
	ParticipantsBoxController::Role::Members,
	AdminDoneCallback(),
	BannedDoneCallback())
, _callback(std::move(callback)) {
	_excludeSelf = false;
}

void SearchFromController::prepare() {
	AddSpecialBoxController::prepare();
	delegate()->peerListSetTitle(tr::lng_search_messages_from());
	if (const auto megagroup = peer()->asMegagroup()) {
		if (!delegate()->peerListFindRow(megagroup->id.value)) {
			delegate()->peerListAppendRow(
				std::make_unique<PeerListRow>(megagroup));
			setDescriptionText({});
			delegate()->peerListRefreshRows();
		}
	}
	prependChannelRows();
}

void SearchFromController::prependChannelRows() {
	auto channels = std::vector<not_null<ChannelData*>>();
	const auto add = [&](PeerData *from) {
		const auto channel = from ? from->asChannel() : nullptr;
		if (channel
			&& channel != peer()
			&& !ranges::contains(channels, not_null(channel))
			&& !delegate()->peerListFindRow(channel->id.value)) {
			channels.push_back(channel);
		}
	};
	if (const auto megagroup = peer()->asMegagroup()) {
		add(megagroup->discussionLink());
	}
	for (const auto &sendAs : session().sendAsPeers().list(peer())) {
		add(sendAs.peer);
	}
	if (const auto history = peer()->owner().historyLoaded(peer())) {
		for (const auto &block : history->blocks) {
			for (const auto &view : block->messages) {
				add(view->data()->from());
			}
		}
	}
	if (channels.empty()) {
		return;
	}
	for (const auto &channel : channels | ranges::views::reverse) {
		delegate()->peerListPrependRow(
			std::make_unique<PeerListRow>(channel));
	}
	setDescriptionText({});
	delegate()->peerListRefreshRows();
}

std::unique_ptr<PeerListRow> SearchFromController::createSearchRow(
		not_null<PeerData*> peer) {
	if (peer->isChannel()) {
		return std::make_unique<PeerListRow>(peer);
	}
	return AddSpecialBoxController::createSearchRow(peer);
}

void SearchFromController::rowClicked(not_null<PeerListRow*> row) {
	if (const auto onstack = base::duplicate(_callback)) {
		onstack(row->peer());
	}
}

} // namespace Dialogs
