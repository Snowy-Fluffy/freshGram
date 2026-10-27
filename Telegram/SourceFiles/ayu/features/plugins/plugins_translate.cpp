// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_translate.h"

#include "ayu/features/plugins/plugins_service.h"
#include "base/weak_ptr.h"
#include "crl/crl_on_main.h"
#include "main/main_session.h"
#include "ui/text/text_utilities.h"

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/manager.h"
#endif

namespace FreshPlugins {

#ifdef FRESHGRAM_PLUGINS

namespace {

constexpr auto kTranslateTimeoutMs = 20000;
constexpr auto kBatchItems = size_t(20);
constexpr auto kBatchBytes = size_t(24) << 10;

[[nodiscard]] bool Provides(
		const fgpl::ManagerSnapshot &snapshot,
		const std::string &id) {
	const auto info = snapshot.plugin(id);
	const auto parsed = snapshot.manifest(id);
	const auto grant = snapshot.grantsOf(id);
	return info
		&& info->enabled
		&& parsed
		&& parsed->translator.declared
		&& grant
		&& grant->granted.contains("translate.provide");
}

[[nodiscard]] QString NameOf(const fgpl::Manifest &manifest) {
	const auto &declared = manifest.translator.name;
	return QString::fromStdString(declared.empty()
		? manifest.id
		: declared);
}

class PluginTranslateProvider final
	: public Ui::TranslateProvider
	, public base::has_weak_ptr {
public:
	PluginTranslateProvider(
		not_null<Main::Session*> session,
		std::string plugin)
	: _account(AccountId(session).toStdString())
	, _plugin(std::move(plugin)) {
	}

	[[nodiscard]] bool supportsMessageId() const override {
		return false;
	}

	void request(
			Ui::TranslateProviderRequest request,
			LanguageId to,
			Fn<void(Ui::TranslateProviderResult)> done) override {
		auto list = std::vector<Ui::TranslateProviderRequest>();
		list.push_back(std::move(request));
		requestBatch(
			std::move(list),
			to,
			[done](int, Ui::TranslateProviderResult result) {
				done(std::move(result));
			},
			[] {});
	}

	void requestBatch(
			std::vector<Ui::TranslateProviderRequest> requests,
			const LanguageId &to,
			Fn<void(int, Ui::TranslateProviderResult)> doneOne,
			Fn<void()> doneAll) override {
		if (requests.empty()) {
			doneAll();
			return;
		}
		struct State {
			int remaining = 0;
			Fn<void(int, Ui::TranslateProviderResult)> doneOne;
			Fn<void()> doneAll;
		};
		auto texts = std::vector<std::string>();
		texts.reserve(requests.size());
		for (const auto &request : requests) {
			texts.push_back(request.text.text.toStdString());
		}
		auto chunks = std::vector<std::vector<int>>();
		auto bytes = kBatchBytes;
		for (auto i = 0; i != int(texts.size()); ++i) {
			const auto size = texts[i].size();
			if (chunks.empty()
				|| chunks.back().size() >= kBatchItems
				|| size > bytes) {
				chunks.emplace_back();
				bytes = kBatchBytes;
			}
			chunks.back().push_back(i);
			bytes -= std::min(bytes, size);
		}
		const auto state = std::make_shared<State>(State{
			.remaining = int(chunks.size()),
			.doneOne = std::move(doneOne),
			.doneAll = std::move(doneAll),
		});
		const auto target = to.twoLetterCode().toStdString();
		const auto weak = base::make_weak(this);
		const auto limit = size_t(MaxChars());
		for (auto &chunk : chunks) {
			auto items = std::vector<std::string>();
			for (const auto index : chunk) {
				items.push_back(texts[index]);
			}
			const auto finish = [=](
					std::vector<int> indices,
					fgpl::TranslateResult result) {
				const auto good = result.error.empty()
					&& (result.items.size() == indices.size());
				for (auto k = 0; k != int(indices.size()); ++k) {
					auto one = Ui::TranslateProviderResult{
						.error = Ui::TranslateProviderError::Unknown,
					};
					if (good && !result.items[k].empty()) {
						auto text = TextWithEntities{
							QString::fromStdString(result.items[k]),
						};
						if (result.markdown) {
							TextUtilities::ParseEntities(text, TextParseMarkdown);
						}
						one = Ui::TranslateProviderResult{ .text = std::move(text) };
					}
					state->doneOne(indices[k], std::move(one));
				}
				if (!--state->remaining) {
					state->doneAll();
				}
			};
			const auto tooLong = ranges::any_of(chunk, [&](int index) {
				return limit && size_t(requests[index].text.text.size()) > limit;
			});
			if (tooLong || to.twoLetterCode().isEmpty()) {
				auto failed = fgpl::TranslateResult();
				failed.error = "invalid";
				finish(chunk, std::move(failed));
				continue;
			}
			const auto account = _account;
			const auto plugin = _plugin;
			const auto indices = chunk;
			const auto posted = Post([=, items = std::move(items)](
					not_null<fgpl::PluginManager*> manager) mutable {
				manager->translateAsync(
					account,
					plugin,
					fgpl::TranslateRequest{ target, std::move(items) },
					kTranslateTimeoutMs,
					[=](fgpl::TranslateResult result) {
						crl::on_main(weak, [=, result = std::move(result)]() mutable {
							finish(indices, std::move(result));
						});
					});
			}, false);
			if (!posted) {
				auto failed = fgpl::TranslateResult();
				failed.error = "unavailable";
				finish(chunk, std::move(failed));
			}
		}
	}

private:
	[[nodiscard]] int MaxChars() const {
		const auto snapshot = Snapshot();
		const auto parsed = snapshot ? snapshot->manifest(_plugin) : nullptr;
		return parsed ? parsed->translator.maxChars : 0;
	}

	std::string _account;
	std::string _plugin;

};

} // namespace

std::vector<TranslatorEntry> Translators() {
	auto result = std::vector<TranslatorEntry>();
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

bool TranslatorAvailable(const QString &plugin) {
	const auto snapshot = Snapshot();
	return snapshot && Provides(*snapshot, plugin.toStdString());
}

QString TranslatorName(const QString &plugin) {
	const auto snapshot = Snapshot();
	const auto id = plugin.toStdString();
	const auto parsed = snapshot ? snapshot->manifest(id) : nullptr;
	return parsed ? NameOf(*parsed) : plugin;
}

std::unique_ptr<Ui::TranslateProvider> CreatePluginTranslateProvider(
		not_null<Main::Session*> session,
		const QString &plugin) {
	if (!TranslatorAvailable(plugin)) {
		return nullptr;
	}
	return std::make_unique<PluginTranslateProvider>(
		session,
		plugin.toStdString());
}

#else

std::vector<TranslatorEntry> Translators() {
	return {};
}

bool TranslatorAvailable(const QString &) {
	return false;
}

QString TranslatorName(const QString &plugin) {
	return plugin;
}

std::unique_ptr<Ui::TranslateProvider> CreatePluginTranslateProvider(
		not_null<Main::Session*>,
		const QString &) {
	return nullptr;
}

#endif

} // namespace FreshPlugins
