// This is the source code of freshGram.
#include "ayu/ui/boxes/story_privacy_box.h"

#include "boxes/peer_list_controllers.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace AyuUi {
namespace {

class Controller final : public ContactsBoxController {
public:
	Controller(
		not_null<Main::Session*> session,
		rpl::producer<QString> title,
		std::vector<not_null<PeerData*>> selected)
	: ContactsBoxController(session)
	, _title(std::move(title))
	, _selected(std::move(selected)) {
	}

	void rowClicked(not_null<PeerListRow*> row) override {
		delegate()->peerListSetRowChecked(row, !row->checked());
	}

protected:
	void prepareViewHook() override {
		delegate()->peerListSetTitle(std::move(_title));
		delegate()->peerListAddSelectedPeers(base::take(_selected));
	}

private:
	rpl::producer<QString> _title;
	std::vector<not_null<PeerData*>> _selected;

};

[[nodiscard]] QString AudienceName(FreshStories::Audience audience) {
	using FreshStories::Audience;
	switch (audience) {
	case Audience::Everyone: return tr::fresh_StoryEveryone(tr::now);
	case Audience::CloseFriends: return tr::fresh_StoryCloseFriends(tr::now);
	case Audience::Selected: return tr::fresh_StorySelected(tr::now);
	case Audience::Contacts: break;
	}
	return tr::fresh_StoryContacts(tr::now);
}

void FillStoryAudienceBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		StoryAudienceChoice current,
		Fn<void(StoryAudienceChoice)> done) {
	using FreshStories::Audience;

	box->setTitle(tr::fresh_StoryAudienceTitle());
	const auto state = box->lifetime().make_state<StoryAudienceChoice>(
		std::move(current));
	const auto changed = box->lifetime().make_state<rpl::event_stream<>>();
	const auto container = box->verticalLayout();

	const auto group = std::make_shared<Ui::RadiobuttonGroup>(
		int(state->audience));
	Ui::AddSkip(container);
	for (const auto audience : {
			Audience::Everyone,
			Audience::Contacts,
			Audience::CloseFriends,
			Audience::Selected }) {
		container->add(
			object_ptr<Ui::Radiobutton>(
				container,
				group,
				int(audience),
				AudienceName(audience),
				st::messagePrivacyCheck),
			st::settingsSendTypePadding);
	}
	group->setChangedCallback([=](int value) {
		state->audience = Audience(value);
		changed->fire({});
	});
	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);

	const auto addUsers = [&](
			rpl::producer<QString> text,
			const style::icon *icon,
			bool selected) {
		const auto wrap = container->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				container,
				object_ptr<Ui::VerticalLayout>(container)));
		auto count = changed->events_starting_with({}) | rpl::map([=] {
			const auto &list = selected ? state->selected : state->excepted;
			return list.empty() ? QString() : QString::number(list.size());
		});
		const auto button = Settings::AddButtonWithLabel(
			wrap->entity(),
			std::move(text),
			std::move(count),
			st::settingsButton,
			{ icon });
		button->setClickedCallback([=] {
			ShowStoryUsersBox(
				controller,
				selected
					? tr::fresh_StorySelectedTitle()
					: tr::fresh_StoryExceptTitle(),
				selected ? state->selected : state->excepted,
				crl::guard(box, [=](std::vector<not_null<UserData*>> users) {
					(selected ? state->selected : state->excepted)
						= std::move(users);
					changed->fire({});
				}));
		});
		wrap->toggleOn(changed->events_starting_with({}) | rpl::map([=] {
			const auto audience = state->audience;
			return selected
				? (audience == Audience::Selected)
				: (audience == Audience::Everyone
					|| audience == Audience::Contacts);
		}));
		wrap->finishAnimating();
	};
	addUsers(tr::fresh_StoryExceptButton(), &st::menuIconRemove, false);
	addUsers(tr::fresh_StoryChooseButton(), &st::menuIconAdd, true);

	box->addButton(tr::lng_settings_save(), [=] {
		if (state->audience == Audience::Selected
			&& state->selected.empty()) {
			controller->showToast(tr::fresh_StoryNoUsers(tr::now));
			return;
		}
		const auto callback = done;
		auto result = *state;
		box->closeBox();
		callback(std::move(result));
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace

QString StoryAudienceText(const StoryAudienceChoice &choice) {
	using FreshStories::Audience;
	const auto name = AudienceName(choice.audience);
	if (choice.audience == Audience::Selected) {
		return tr::fresh_StorySelectedCount(
			tr::now,
			lt_amount,
			QString::number(choice.selected.size()));
	} else if (choice.audience != Audience::CloseFriends
		&& !choice.excepted.empty()) {
		return tr::fresh_StoryAudienceExcept(
			tr::now,
			lt_audience,
			name,
			lt_amount,
			QString::number(choice.excepted.size()));
	}
	return name;
}

void ShowStoryAudienceBox(
		not_null<Window::SessionController*> controller,
		StoryAudienceChoice current,
		Fn<void(StoryAudienceChoice)> done) {
	controller->show(Box(
		FillStoryAudienceBox,
		controller,
		std::move(current),
		std::move(done)));
}

void ShowStoryUsersBox(
		not_null<Window::SessionController*> controller,
		rpl::producer<QString> title,
		const std::vector<not_null<UserData*>> &selected,
		Fn<void(std::vector<not_null<UserData*>>)> done) {
	auto peers = std::vector<not_null<PeerData*>>();
	peers.reserve(selected.size());
	for (const auto &user : selected) {
		peers.push_back(user);
	}
	auto initBox = [=](not_null<PeerListBox*> box) {
		box->addButton(tr::lng_settings_save(), [=] {
			auto users = std::vector<not_null<UserData*>>();
			for (const auto &peer : box->collectSelectedRows()) {
				if (const auto user = peer->asUser()) {
					users.push_back(user);
				}
			}
			const auto callback = done;
			box->closeBox();
			callback(std::move(users));
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	};
	controller->show(Box<PeerListBox>(
		std::make_unique<Controller>(
			&controller->session(),
			std::move(title),
			std::move(peers)),
		std::move(initBox)));
}

} // namespace AyuUi
