// This is the source code of freshGram.
#include "ayu/features/stories/story_entry.h"

#include "ayu/features/stories/story_poster.h"
#include "ayu/ui/boxes/story_compose_box.h"
#include "core/file_utilities.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "mainwidget.h"
#include "storage/storage_media_prepare.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_menu_icons.h"

namespace FreshStories {
namespace {

[[nodiscard]] bool OneMedia(const Ui::PreparedList &list) {
	if (list.files.size() != 1) {
		return false;
	}
	const auto type = list.files.front().type;
	return (type == Ui::PreparedFile::Type::Photo)
		|| (type == Ui::PreparedFile::Type::Video);
}

void ChooseFile(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto callback = crl::guard(controller, [=](
			FileDialog::OpenResult &&result) {
		const auto check = OneMedia;
		const auto showError = [=](tr::phrase<> text) {
			controller->showToast(text(tr::now));
		};
		auto list = Storage::PreparedFileFromFilesDialog(
			std::move(result),
			check,
			showError,
			st::sendMediaPreviewSize,
			controller->session().premium());
		if (!list) {
			return;
		}
		AyuUi::ShowStoryComposeBox(
			controller,
			peer,
			std::move(list->files.front()));
	});
	FileDialog::GetOpenPath(
		controller->content().get(),
		tr::fresh_StoryChoose(tr::now),
		FileDialog::PhotoVideoFilesFilter(),
		callback);
}

} // namespace

void StartPosting(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	CheckCanPost(
		&controller->session(),
		peer,
		crl::guard(controller, [=] {
			ChooseFile(controller, peer);
		}),
		crl::guard(controller, [=](QString type) {
			controller->showToast(ErrorText(type));
		}));
}

bool HandleStoriesClick(
		not_null<Window::SessionController*> controller,
		uint64 id) {
	const auto self = controller->session().user();
	if (PeerId(int64(id)) != self->id
		|| controller->session().data().stories().source(self->id)) {
		return false;
	}
	StartPosting(controller, self);
	return true;
}

void StartPostingFromUrls(
		not_null<Window::SessionController*> controller,
		const QList<QUrl> &urls) {
	const auto self = controller->session().user();
	if (!self->canPostStories()) {
		return;
	}
	auto list = Storage::PrepareMediaList(
		urls,
		st::sendMediaPreviewSize,
		controller->session().premium());
	if (list.error != Ui::PreparedList::Error::None || !OneMedia(list)) {
		controller->showToast(tr::fresh_StoryChoose(tr::now));
		return;
	}
	const auto file = std::make_shared<Ui::PreparedFile>(
		std::move(list.files.front()));
	CheckCanPost(
		&controller->session(),
		self,
		crl::guard(controller, [=] {
			AyuUi::ShowStoryComposeBox(controller, self, std::move(*file));
		}),
		crl::guard(controller, [=](QString type) {
			controller->showToast(ErrorText(type));
		}));
}

void AddPostStoryAction(
		PeerData *peer,
		not_null<Window::SessionController*> controller,
		const Window::PeerMenuCallback &addCallback) {
	if (!peer || !peer->canPostStories()) {
		return;
	}
	const auto strong = not_null<PeerData*>(peer);
	addCallback(tr::fresh_StoryAdd(tr::now), [=] {
		StartPosting(controller, strong);
	}, &st::menuIconAdd);
}

} // namespace FreshStories
