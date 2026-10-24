/*
 * This is the source code of AyuGram for Desktop.
 *
 * We do not and cannot prevent the use of our code,
 * but be respectful and credit the original author.
 *
 * Copyright @Radolyn, 2025
 */
#include "ayu/features/translator/compose_translate.h"

#include "ayu/ayu_settings.h"
#include "ayu/secret/secret_peer.h"
#include "data/data_peer.h"
#include "lang/lang_keys.h"
#include "lang/translate_provider.h"
#include "main/main_session.h"
#include "spellcheck/spellcheck_types.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/show.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "ayu/ui/settings/settings_chats.h"

#include <vector>

namespace Ayu {

namespace {

constexpr auto kRequestTimeout = 20 * crl::time(1000);
constexpr auto kProgressDelay = crl::time(600);

[[nodiscard]] bool IsOpaqueEntity(EntityType type) {
	switch (type) {
	case EntityType::Url:
	case EntityType::Email:
	case EntityType::Hashtag:
	case EntityType::Cashtag:
	case EntityType::Mention:
	case EntityType::MentionName:
	case EntityType::CustomEmoji:
	case EntityType::BotCommand:
	case EntityType::Phone:
	case EntityType::BankCard:
		return true;
	}
	return false;
}

[[nodiscard]] bool IsOpaqueToken(QStringView token) {
	if (token.isEmpty()) {
		return true;
	}
	const auto first = token.front();
	return (first == '@')
		|| (first == '#')
		|| (first == '$')
		|| token.startsWith(u"http://"_q, Qt::CaseInsensitive)
		|| token.startsWith(u"https://"_q, Qt::CaseInsensitive)
		|| token.startsWith(u"www."_q, Qt::CaseInsensitive)
		|| token.startsWith(u"tg://"_q, Qt::CaseInsensitive);
}

} // namespace

bool WorthTranslating(const TextWithEntities &text) {
	const auto &plain = text.text;
	const auto trimmed = QStringView(plain).trimmed();
	if (trimmed.size() > 1
		&& trimmed.front() == '/'
		&& trimmed[1].isLetter()) {
		return false;
	}
	auto masked = std::vector<bool>(size_t(plain.size()), false);
	for (const auto &entity : text.entities) {
		if (!IsOpaqueEntity(entity.type())) {
			continue;
		}
		const auto till = std::min(
			entity.offset() + entity.length(),
			int(plain.size()));
		for (auto i = std::max(entity.offset(), 0); i < till; ++i) {
			masked[i] = true;
		}
	}
	auto from = 0;
	const auto size = int(plain.size());
	while (from < size) {
		while (from < size && plain[from].isSpace()) {
			++from;
		}
		auto till = from;
		while (till < size && !plain[till].isSpace()) {
			++till;
		}
		if (till > from
			&& !IsOpaqueToken(QStringView(plain).mid(from, till - from))) {
			for (auto i = from; i != till; ++i) {
				if (!masked[i] && plain[i].isLetter()) {
					return true;
				}
			}
		}
		from = till;
	}
	return false;
}

ComposeTranslator::ComposeTranslator() = default;

ComposeTranslator::~ComposeTranslator() = default;

void OpenTranslateSettings(not_null<Window::SessionController*> controller) {
	controller->window().setHighlightControlId(
		u"ayu/showTranslateButtonInMessageField"_q);
	controller->showSettings(Settings::AyuChatsId());
}

bool ComposeTranslator::busy() const {
	return _busy;
}

void ComposeTranslator::finish() {
	_busy = false;
	_timeout.cancel();
	_progressDelay.cancel();
	if (const auto toast = _progress.get()) {
		toast->hide();
	}
	_progress = nullptr;
}

void ComposeTranslator::cancel() {
	if (!_busy) {
		return;
	}
	++_requestId;
	finish();
}

void ComposeTranslator::remember(const QString &translated) {
	_remembered = translated;
}

void ComposeTranslator::textChanged(const QString &text) {
	if (text != _remembered) {
		_remembered = QString();
	}
}

bool ComposeTranslator::isRemembered(const QString &text) const {
	return !_remembered.isEmpty() && (text == _remembered);
}

void ComposeTranslator::start(
		std::shared_ptr<Ui::Show> show,
		not_null<PeerData*> peer,
		TextWithEntities text,
		Fn<void(std::optional<TextWithEntities>)> done,
		bool silent) {
	if (_busy) {
		return;
	}
	if (text.text.trimmed().isEmpty()) {
		show->showToast(tr::fresh_TranslateComposeEmpty(tr::now));
		return;
	}
	if (!FreshSecret::IsSecretPeerId(peer->id)
		|| Ui::TranslateProviderIsLocal()) {
		request(
			std::move(show),
			peer,
			std::move(text),
			std::move(done),
			silent);
		return;
	}
	// Secret chats: the text is sent to the provider's server, so ask first.
	const auto weak = base::make_weak(this);
	const auto providerIdentity = Ui::TranslateProviderIdentity();
	show->showBox(Ui::MakeConfirmBox({
		.text = tr::fresh_SecretTranslateWarning(
			tr::now,
			lt_provider,
			Ui::TranslateProviderName()),
		.confirmed = [=, text = std::move(text), done = std::move(done)](
				Fn<void()> close) mutable {
			close();
			const auto strong = weak.get();
			if (!strong
				|| strong->_busy
				|| Ui::TranslateProviderIdentity() != providerIdentity) {
				return;
			}
			strong->request(
				show,
				peer,
				std::move(text),
				std::move(done),
				silent);
		},
		.confirmText = tr::lng_context_translate(tr::now),
	}));
}

void ComposeTranslator::request(
		std::shared_ptr<Ui::Show> show,
		not_null<PeerData*> peer,
		TextWithEntities text,
		Fn<void(std::optional<TextWithEntities>)> done,
		bool silent) {
	const auto identity = Ui::TranslateProviderIdentity();
	if (!_provider || _providerIdentity != identity) {
		_provider = Ui::CreateTranslateProvider(&peer->session());
		_providerIdentity = identity;
	}
	if (!_provider) {
		show->showToast(tr::fresh_TranslateComposeFailed(tr::now));
		done(std::nullopt);
		return;
	}
	_busy = true;
	const auto id = ++_requestId;
	const auto weak = base::make_weak(this);
	const auto showProgress = [=] {
		if (const auto strong = weak.get()) {
			strong->_progress = show->showToast(Ui::Toast::Config{
				.text = TextWithEntities{
					tr::fresh_TranslateComposeProgress(tr::now),
				},
				.infinite = true,
			});
		}
	};
	if (silent) {
		_progressDelay.setCallback(showProgress);
		_progressDelay.callOnce(kProgressDelay);
	} else {
		showProgress();
	}
	const auto fail = [=] {
		show->showToast(silent
			? tr::fresh_AutoTranslateFailed(tr::now)
			: tr::fresh_TranslateComposeFailed(tr::now));
		done(std::nullopt);
	};
	_timeout.setCallback([=] {
		const auto strong = weak.get();
		if (!strong || strong->_requestId != id) {
			return;
		}
		++strong->_requestId;
		strong->finish();
		fail();
	});
	_timeout.callOnce(kRequestTimeout);
	_provider->request(
		Ui::TranslateProviderRequest{ .text = std::move(text) },
		LanguageId::FromName(AyuSettings::getInstance().translateComposeTo()),
		[=](Ui::TranslateProviderResult result) {
			const auto strong = weak.get();
			if (!strong || strong->_requestId != id) {
				return;
			}
			strong->finish();
			if (result.error == Ui::TranslateProviderError::LocalLanguagePackMissing
				&& Ui::TranslateProviderIsLocal()) {
				show->showToast(tr::fresh_TranslateLocalMissing(tr::now));
				done(std::nullopt);
				return;
			}
			if (result.error != Ui::TranslateProviderError::None
				|| !result.text
				|| result.text->text.trimmed().isEmpty()) {
				fail();
				return;
			}
			if (!silent) {
				show->showToast(tr::fresh_TranslateComposeDone(tr::now));
			}
			done(std::move(result.text));
		});
}

} // namespace Ayu
