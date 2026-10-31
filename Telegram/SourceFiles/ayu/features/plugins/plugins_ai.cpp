// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_ai.h"

#include "ayu/features/plugins/plugins_service.h"
#include "crl/crl_on_main.h"
#include "main/main_session.h"
#include "ui/text/text_utilities.h"

#include <atomic>
#include <map>
#include <memory>

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/manager.h"
#endif

namespace FreshPlugins {

#ifdef FRESHGRAM_PLUGINS

namespace {

constexpr auto kAiTimeoutMs = 60000;

struct Pending {
	std::atomic<int64_t> ticket = 0;
	std::atomic<bool> cancelled = false;
};

[[nodiscard]] std::map<int, std::shared_ptr<Pending>> &Active() {
	static auto result = std::map<int, std::shared_ptr<Pending>>();
	return result;
}

[[nodiscard]] bool Provides(
		const fgpl::ManagerSnapshot &snapshot,
		const std::string &id) {
	const auto info = snapshot.plugin(id);
	const auto parsed = snapshot.manifest(id);
	const auto grant = snapshot.grantsOf(id);
	return info
		&& info->enabled
		&& parsed
		&& parsed->aiProvider.declared
		&& grant
		&& grant->granted.contains("ai.provide");
}

[[nodiscard]] QString NameOf(const fgpl::Manifest &manifest) {
	const auto &declared = manifest.aiProvider.name;
	return QString::fromStdString(declared.empty()
		? manifest.id
		: declared);
}

} // namespace

std::vector<AiProviderEntry> AiProviders() {
	auto result = std::vector<AiProviderEntry>();
	const auto snapshot = Snapshot();
	if (!snapshot) {
		return result;
	}
	for (const auto &info : snapshot->plugins) {
		if (!Provides(*snapshot, info.id)) {
			continue;
		}
		result.push_back({
			QString::fromStdString(info.id),
			NameOf(*snapshot->manifest(info.id)),
		});
	}
	return result;
}

bool AiAvailable(const QString &plugin) {
	const auto snapshot = Snapshot();
	return snapshot && Provides(*snapshot, plugin.toStdString());
}

QString AiProviderName(const QString &plugin) {
	const auto snapshot = Snapshot();
	const auto parsed = snapshot ? snapshot->manifest(plugin.toStdString()) : nullptr;
	return parsed ? NameOf(*parsed) : plugin;
}

int RequestAi(
		not_null<Main::Session*> session,
		const QString &plugin,
		AiRequestData data,
		Fn<void(TextWithEntities &&)> done,
		Fn<void(const QString &)> fail) {
	static auto lastId = 0;
	const auto id = --lastId;
	const auto state = std::make_shared<Pending>();
	Active().emplace(id, state);

	const auto finish = [=](fgpl::AiResult result) {
		const auto i = Active().find(id);
		if (i == Active().end() || state->cancelled) {
			return;
		}
		Active().erase(i);
		if (!result.error.empty() || result.text.empty()) {
			fail(QString::fromStdString(result.error.empty()
				? std::string("empty")
				: result.error));
			return;
		}
		auto text = TextWithEntities{ QString::fromStdString(result.text) };
		if (result.markdown) {
			TextUtilities::ParseEntities(text, TextParseMarkdown);
		}
		done(std::move(text));
	};
	const auto unavailable = [=] {
		crl::on_main([=] {
			auto result = fgpl::AiResult();
			result.error = "unavailable";
			finish(std::move(result));
		});
	};

	auto request = fgpl::AiRequest();
	request.text = data.text.text.toStdString();
	request.translateTo = data.translateTo.toStdString();
	request.tone = data.tone.toStdString();
	request.prompt = data.prompt.toStdString();
	request.proofread = data.proofread;
	request.emojify = data.emojify;
	const auto account = AccountId(session).toStdString();
	const auto pluginId = plugin.toStdString();
	const auto posted = Post([=, request = std::move(request)](
			not_null<fgpl::PluginManager*> manager) mutable {
		const auto ticket = manager->aiAsync(
			account,
			pluginId,
			std::move(request),
			kAiTimeoutMs,
			[=](fgpl::AiResult result) {
				crl::on_main([=, result = std::move(result)]() mutable {
					finish(std::move(result));
				});
			});
		state->ticket = ticket;
		if (state->cancelled && ticket) {
			manager->cancelAi(ticket);
		}
	}, false);
	if (!posted) {
		unavailable();
	}
	return id;
}

void CancelAi(int id) {
	const auto i = Active().find(id);
	if (i == Active().end()) {
		return;
	}
	const auto state = i->second;
	Active().erase(i);
	state->cancelled = true;
	if (const auto ticket = state->ticket.load()) {
		Post([=](not_null<fgpl::PluginManager*> manager) {
			manager->cancelAi(ticket);
		}, false);
	}
}

#else

std::vector<AiProviderEntry> AiProviders() {
	return {};
}

bool AiAvailable(const QString &) {
	return false;
}

QString AiProviderName(const QString &plugin) {
	return plugin;
}

int RequestAi(
		not_null<Main::Session*>,
		const QString &,
		AiRequestData,
		Fn<void(TextWithEntities &&)>,
		Fn<void(const QString &)> fail) {
	crl::on_main([=] {
		fail(u"unavailable"_q);
	});
	return 0;
}

void CancelAi(int) {
}

#endif

} // namespace FreshPlugins
