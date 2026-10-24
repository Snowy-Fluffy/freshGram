/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lang/translate_provider.h"

#include "base/options.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_msg_id.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "lang/translate_mtproto_provider.h"
#include "lang/translate_url_provider.h"
#include "platform/platform_translate_provider.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/recognition/text_translation.h"
#include "ayu/features/translator/ayu_translate_provider.h"
#include "ayu/features/translator/local_translate_provider.h"


namespace {

base::options::option<QString> OptionTranslateUrlTemplate({
	.id = "translate-url-template",
	.name = "Translate URL template",
	.description = "Template URL for custom translation provider."
		" Supports %q text, %f source language and %t target language.",
});

[[nodiscard]] TranslationProvider ResolveTranslateProvider() {
	const auto provider = AyuSettings::getInstance().translationProvider();
	if ((provider == TranslationProvider::Native)
		&& !Platform::IsTranslateProviderAvailable()) {
		return TranslationProvider::Telegram;
	}
	if ((provider == TranslationProvider::Local)
		&& !FreshRecognition::TextTranslationAvailable()) {
		return TranslationProvider::Telegram;
	}
	return provider;
}

[[nodiscard]] QString ProviderIdentity(
		TranslationProvider provider,
		const QString &urlTemplate) {
	return (!urlTemplate.isEmpty() && urlTemplate.contains(u"%q"_q))
		? (u"url:"_q + urlTemplate)
		: (u"provider:"_q + QString::number(int(provider)));
}

} // namespace

namespace Ui {

std::unique_ptr<TranslateProvider> CreateTranslateProvider(
		not_null<Main::Session*> session,
		const QString &requiredIdentity) {
	const auto urlTemplate = OptionTranslateUrlTemplate.value();
	const auto provider = ResolveTranslateProvider();
	if (!requiredIdentity.isEmpty()
		&& ProviderIdentity(provider, urlTemplate) != requiredIdentity) {
		return nullptr;
	}
	if (!urlTemplate.isEmpty()
		&& urlTemplate.contains(u"%q"_q)) {
		return CreateUrlTranslateProvider(urlTemplate);
	}
	switch (provider) {
	case TranslationProvider::Google:
	case TranslationProvider::Yandex:
		return CreateAyuTranslateProvider(session, provider);
	case TranslationProvider::Native:
		if (auto native = Platform::CreateTranslateProvider()) {
			return native;
		}
		if (!requiredIdentity.isEmpty()) {
			return nullptr;
		}
		break;
	case TranslationProvider::Local:
		return Ayu::Translator::CreateLocalTranslateProvider();
	case TranslationProvider::Telegram:
		break;
	}
	return CreateMTProtoTranslateProvider(session);
}

QString TranslateProviderIdentity() {
	return ProviderIdentity(
		ResolveTranslateProvider(),
		OptionTranslateUrlTemplate.value());
}

bool TranslateProviderIsLocal() {
	const auto urlTemplate = OptionTranslateUrlTemplate.value();
	return (urlTemplate.isEmpty() || !urlTemplate.contains(u"%q"_q))
		&& (ResolveTranslateProvider() == TranslationProvider::Local);
}

QString TranslateProviderName() {
	const auto urlTemplate = OptionTranslateUrlTemplate.value();
	if (!urlTemplate.isEmpty() && urlTemplate.contains(u"%q"_q)) {
		return tr::fresh_SecretTranslateCustomServer(tr::now);
	}
	switch (ResolveTranslateProvider()) {
	case TranslationProvider::Google:
		return u"Google"_q;
	case TranslationProvider::Yandex:
		return u"Yandex"_q;
	case TranslationProvider::Native:
		if (!Platform::IsTranslateProviderAvailable()) {
			return u"Telegram"_q;
		}
		return tr::fresh_SecretTranslateSystem(tr::now);
	case TranslationProvider::Local:
		return tr::fresh_TranslateProviderLocal(tr::now);
	case TranslationProvider::Telegram:
		return u"Telegram"_q;
	}
	return u"Telegram"_q;
}

TranslateProviderRequest PrepareTranslateProviderRequest(
		not_null<TranslateProvider*> provider,
		not_null<PeerData*> peer,
		MsgId msgId,
		TextWithEntities text) {
	auto result = TranslateProviderRequest{
		.peerId = uint64(peer->id.value),
		.msgId = IsServerMsgId(msgId) ? msgId.bare : 0,
		.text = std::move(text),
	};
	if (provider->supportsMessageId()) {
		return result;
	}
	if (result.msgId) {
		if (const auto i = peer->owner().message(peer, MsgId(result.msgId))) {
			result.text = i->originalText();
		}
		result.msgId = 0;
	}
	return result;
}

} // namespace Ui
