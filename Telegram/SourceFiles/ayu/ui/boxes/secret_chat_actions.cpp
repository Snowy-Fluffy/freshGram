#include "ayu/ui/boxes/secret_chat_actions.h"

#include "ayu/secret/secret_bridge.h"
#include "ayu/secret/secret_manager.h"
#include "ayu/secret/secret_peer.h"
#include "ayu/ui/boxes/secret_key_box.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/checkbox.h"
#include "window/window_session_controller.h"

namespace Ui {
namespace {

[[nodiscard]] QString TtlText(int seconds) {
	if (seconds <= 0) {
		return tr::fresh_SecretTimerOff(tr::now);
	} else if (seconds < 60) {
		return QString("%1 s").arg(seconds);
	} else if (seconds < 3600) {
		return QString("%1 min").arg(seconds / 60);
	} else if (seconds < 86400) {
		return QString("%1 h").arg(seconds / 3600);
	} else if (seconds < 7 * 86400) {
		return QString("%1 d").arg(seconds / 86400);
	}
	return QString("%1 w").arg(seconds / (7 * 86400));
}

} // namespace

void FillSecretTimerBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		int chatId,
		int current) {
	box->setTitle(tr::fresh_SecretTimer());
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	const auto group = std::make_shared<Ui::RadiobuttonGroup>(current);
	auto options = std::vector<int>{
		0, 1, 2, 3, 4, 5, 6, 7, 15, 30, 60, 3600, 86400, 7 * 86400
	};
	if (current > 0
		&& std::find(options.begin(), options.end(), current) == options.end()) {
		options.insert(options.begin() + 1, current);
	}
	for (const auto value : options) {
		box->addRow(
			object_ptr<Ui::Radiobutton>(
				box,
				group,
				value,
				TtlText(value),
				st::defaultCheckbox),
			st::boxRowPadding);
	}
	group->setChangedCallback([=](int value) {
		FreshSecret::Get(session).setTtl(chatId, value);
		box->closeBox();
	});
}

void ShowSecretTimer(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto chatId = FreshSecret::ChatIdOfPeer(peer);
	const auto session = &controller->session();
	const auto info = FreshSecret::Get(session).chat(chatId);
	if (!info || info->state != FreshSecret::ChatState::Ready) {
		return;
	}
	controller->show(Box(FillSecretTimerBox, session, chatId, info->ttl));
}

void ShowSecretKey(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto chatId = FreshSecret::ChatIdOfPeer(peer);
	const auto info = FreshSecret::Get(&controller->session()).chat(chatId, true);
	if (!info || info->keyHash.empty()) {
		return;
	}
	controller->show(Box(
		FillSecretKeyBox,
		info->keyHash));
}

void ConfirmSecretClear(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto weak = base::make_weak(controller);
	const auto history = peer->owner().history(peer);
	controller->show(Ui::MakeConfirmBox({
		.text = tr::fresh_SecretClearAsk(),
		.confirmed = [=](Fn<void()> close) {
			FreshSecret::BridgeFor(&history->session()).historyClearedByUser(
				history);
			close();
		},
	}));
}

void ConfirmSecretEnd(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto chatId = FreshSecret::ChatIdOfPeer(peer);
	const auto session = &controller->session();
	const auto info = FreshSecret::Get(session).chat(chatId);
	const auto ended = info
		&& (info->state == FreshSecret::ChatState::Discarded);
	controller->show(Ui::MakeConfirmBox({
		.text = ended
			? tr::fresh_SecretChatDeleteAsk()
			: tr::fresh_SecretChatEndAsk(),
		.confirmed = [=](Fn<void()> close) {
			FreshSecret::Get(session).end(chatId);
			close();
		},
	}));
}

} // namespace Ui
