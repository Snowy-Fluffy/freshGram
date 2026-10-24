// This is the source code of freshGram.
#include "ayu/ui/boxes/delete_own_topics_box.h"

#include "boxes/peer_list_box.h"
#include "boxes/peer_list_controllers.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_changes.h"
#include "data/data_session.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_row.h"
#include "lang/lang_keys.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "ui/layers/show.h"
#include "styles/style_dialogs.h"

namespace AyuUi {
namespace {

class TopicsController final : public PeerListController {
public:
	TopicsController(
		not_null<Data::Forum*> forum,
		base::flat_set<PeerListRowId> selected)
	: PeerListController(std::make_unique<ChooseTopicSearchController>(forum))
	, _forum(forum)
	, _selected(std::move(selected)) {
		setStyleOverrides(&st::chooseTopicList);

		_forum->chatsListChanges(
		) | rpl::on_next([=] {
			refreshRows();
		}, lifetime());

		_forum->topicDestroyed(
		) | rpl::on_next([=](not_null<Data::ForumTopic*> topic) {
			const auto id = PeerListRowId(topic->rootId().bare);
			if (const auto row = delegate()->peerListFindRow(id)) {
				delegate()->peerListRemoveRow(row);
				delegate()->peerListRefreshRows();
			}
		}, lifetime());
	}

	Main::Session &session() const override {
		return _forum->session();
	}

	void prepare() override {
		delegate()->peerListSetTitle(tr::fresh_DeleteOwnMessagesPickTopics());
		setSearchNoResultsText(tr::lng_topics_not_found(tr::now));
		delegate()->peerListSetSearchMode(PeerListSearchMode::Enabled);
		refreshRows();
	}

	void rowClicked(not_null<PeerListRow*> row) override {
		delegate()->peerListSetRowChecked(row, !row->checked());
	}

	void loadMoreRows() override {
		_forum->requestTopics();
	}

	std::unique_ptr<PeerListRow> createSearchRow(
			PeerListRowId id) override {
		if (const auto topic = _forum->topicFor(MsgId(id))) {
			return ChooseTopicBoxController::MakeRow(topic);
		}
		return nullptr;
	}

private:
	void refreshRows() {
		auto added = false;
		for (const auto &row : _forum->topicsList()->indexed()->all()) {
			const auto topic = row->topic();
			if (!topic) {
				continue;
			}
			const auto id = PeerListRowId(topic->rootId().bare);
			if (delegate()->peerListFindRow(id)) {
				continue;
			}
			delegate()->peerListAppendRow(
				ChooseTopicBoxController::MakeRow(topic));
			if (_selected.contains(id)) {
				if (const auto created = delegate()->peerListFindRow(id)) {
					delegate()->peerListSetRowChecked(created, true);
				}
			}
			added = true;
		}
		if (added) {
			delegate()->peerListRefreshRows();
		}
	}

	const not_null<Data::Forum*> _forum;
	base::flat_set<PeerListRowId> _selected;

};

} // namespace

void ShowDeleteOwnTopicsBox(
		not_null<Data::Forum*> forum,
		std::shared_ptr<Ui::Show> show,
		std::vector<MsgId> current,
		Fn<void(std::vector<MsgId>)> chosen) {
	auto selected = base::flat_set<PeerListRowId>();
	for (const auto id : current) {
		selected.emplace(PeerListRowId(id.bare));
	}
	auto initBox = [=](not_null<PeerListBox*> box) {
		box->addButton(tr::lng_box_done(), [=] {
			auto result = std::vector<MsgId>();
			for (const auto id : box->collectSelectedIds()) {
				result.push_back(MsgId(id));
			}
			box->closeBox();
			chosen(std::move(result));
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		forum->destroyed(
		) | rpl::on_next([=] {
			box->closeBox();
		}, box->lifetime());
	};
	show->showBox(Box<PeerListBox>(
		std::make_unique<TopicsController>(forum, std::move(selected)),
		std::move(initBox)));
}

} // namespace AyuUi
