// This is the source code of freshGram.
#pragma once

#include "window/window_peer_menu.h"

#include <QtCore/QList>
#include <QtCore/QUrl>

class PeerData;

namespace Window {
class SessionController;
} // namespace Window

namespace FreshStories {

// Checks the limit, asks for a photo or a video and opens the window of the story.
void StartPosting(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer);

// A click on my element in the stories list, with no stories to show.
// Returns true when the click is handled here.
bool HandleStoriesClick(
	not_null<Window::SessionController*> controller,
	uint64 id);

// A photo or a video dropped on the stories list is posted as my story.
void StartPostingFromUrls(
	not_null<Window::SessionController*> controller,
	const QList<QUrl> &urls);

void AddPostStoryAction(
	PeerData *peer,
	not_null<Window::SessionController*> controller,
	const Window::PeerMenuCallback &addCallback);

} // namespace FreshStories
