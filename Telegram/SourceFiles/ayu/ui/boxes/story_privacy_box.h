// This is the source code of freshGram.
#pragma once

#include "ayu/features/stories/story_poster.h"

class UserData;

namespace Window {
class SessionController;
} // namespace Window

namespace AyuUi {

// Chooses the contacts to show a story to or to hide it from.
void ShowStoryUsersBox(
	not_null<Window::SessionController*> controller,
	rpl::producer<QString> title,
	const std::vector<not_null<UserData*>> &selected,
	Fn<void(std::vector<not_null<UserData*>>)> done);

struct StoryAudienceChoice {
	FreshStories::Audience audience = FreshStories::Audience::Contacts;
	std::vector<not_null<UserData*>> excepted;
	std::vector<not_null<UserData*>> selected;
};

// The short text of the choice, like "Contacts, except 2".
[[nodiscard]] QString StoryAudienceText(const StoryAudienceChoice &choice);

// Chooses who can see a story, with the people to except or to choose.
void ShowStoryAudienceBox(
	not_null<Window::SessionController*> controller,
	StoryAudienceChoice current,
	Fn<void(StoryAudienceChoice)> done);

} // namespace AyuUi
