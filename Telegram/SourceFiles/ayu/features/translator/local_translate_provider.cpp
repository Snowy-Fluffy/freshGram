// This is the source code of freshGram.
#include "ayu/features/translator/local_translate_provider.h"

#include "ayu/features/recognition/language_id.h"
#include "ayu/features/recognition/recognition_models.h"
#include "ayu/features/recognition/text_translation.h"
#include "ayu/features/translator/translation_markup.h"
#include "base/weak_ptr.h"
#include "spellcheck/platform/platform_language.h"

#include <map>

namespace Ayu::Translator {
namespace {

using namespace FreshRecognition;

[[nodiscard]] MarkupRole RoleOf(int type) {
	switch (EntityType(type)) {
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
	case EntityType::Code:
	case EntityType::Pre:
	case EntityType::MediaTimestamp:
	case EntityType::FormattedDate:
		return MarkupRole::Keep;
	case EntityType::CustomUrl:
	case EntityType::Bold:
	case EntityType::Semibold:
	case EntityType::Italic:
	case EntityType::Underline:
	case EntityType::StrikeOut:
	case EntityType::Blockquote:
	case EntityType::Spoiler:
	case EntityType::Subscript:
	case EntityType::Superscript:
	case EntityType::Marked:
		return MarkupRole::Style;
	case EntityType::Invalid:
	case EntityType::Colorized:
		break;
	}
	return MarkupRole::Drop;
}

[[nodiscard]] MarkupText ToMarkup(const TextWithEntities &source) {
	auto result = MarkupText{ .text = source.text };
	result.spans.reserve(source.entities.size());
	for (const auto &entity : source.entities) {
		result.spans.push_back({
			.offset = entity.offset(),
			.length = entity.length(),
			.type = int(entity.type()),
			.data = entity.data(),
		});
	}
	return result;
}

[[nodiscard]] TextWithEntities FromMarkup(const MarkupRestored &restored) {
	auto result = TextWithEntities{ .text = restored.text.text };
	for (const auto &span : restored.text.spans) {
		result.entities.push_back(EntityInText(
			EntityType(span.type),
			span.offset,
			span.length,
			span.data));
	}
	return result;
}

// A link that lost its words keeps its address, so nothing is lost.
void KeepLostLinks(
		TextWithEntities &text,
		const MarkupMasked &masked,
		const MarkupRestored &restored) {
	for (const auto index : restored.lost) {
		const auto &style = masked.styles[index];
		if (EntityType(style.type) != EntityType::CustomUrl
			|| style.data.isEmpty()) {
			continue;
		}
		text.text += ' ';
		text.entities.push_back(EntityInText(
			EntityType::Url,
			int(text.text.size()),
			int(style.data.size())));
		text.text += style.data;
	}
}

struct Direction {
	std::vector<Model> chain;
	bool missing = false;
};

[[nodiscard]] Direction ChooseDirection(
		const std::vector<Model> &installed,
		const QString &from,
		const QString &to) {
	const auto find = [&](const QString &code) -> const Model* {
		for (const auto &model : installed) {
			if (model.code == code) {
				return &model;
			}
		}
		return nullptr;
	};
	auto result = Direction();
	if (from.isEmpty()) {
		const Model *only = nullptr;
		for (const auto &model : installed) {
			if (model.code.endsWith('-' + to)) {
				if (only) {
					result.missing = true;
					return result;
				}
				only = &model;
			}
		}
		if (only) {
			result.chain.push_back(*only);
		} else {
			result.missing = true;
		}
		return result;
	}
	if (const auto direct = find(from + '-' + to)) {
		result.chain.push_back(*direct);
		return result;
	}
	const auto first = find(from + u"-en"_q);
	const auto second = find(u"en-"_q + to);
	if (first && second && from != u"en"_q && to != u"en"_q) {
		result.chain.push_back(*first);
		result.chain.push_back(*second);
	} else {
		result.missing = true;
	}
	return result;
}

constexpr auto kSureProbability = 0.5f;
constexpr auto kPossibleProbability = 0.1f;
constexpr auto kGuessProbability = 0.3f;

// The languages that can be handled: the sources of the models and the target.
[[nodiscard]] QStringList Candidates(
		const std::vector<Model> &installed,
		const QString &target) {
	auto result = QStringList{ target };
	for (const auto &model : installed) {
		const auto from = model.code.section('-', 0, 0);
		if (!result.contains(from)) {
			result.push_back(from);
		}
	}
	return result;
}

[[nodiscard]] QString DetectLanguage(
		const QString &text,
		const QStringList &candidates) {
	const auto guesses = GuessLanguages(text);
	if (!guesses.empty()) {
		const auto &top = guesses.front();
		if (top.probability >= kSureProbability) {
			return top.code;
		}
		for (const auto &guess : guesses) {
			if (guess.probability >= kPossibleProbability
				&& candidates.contains(guess.code)) {
				return guess.code;
			}
		}
		return (top.probability >= kGuessProbability)
			? top.code
			: QString();
	}
#ifndef TDESKTOP_DISABLE_SPELLCHECK
	const auto detected = Platform::Language::Recognize(text);
	if (detected.known()) {
		return detected.twoLetterCode();
	}
#endif // !TDESKTOP_DISABLE_SPELLCHECK
	return QString();
}

class Provider final
	: public Ui::TranslateProvider
	, public base::has_weak_ptr {
public:
	~Provider() {
		for (const auto &flag : _flags) {
			flag->store(true);
		}
	}

	[[nodiscard]] bool supportsMessageId() const override {
		return false;
	}

	void request(
			Ui::TranslateProviderRequest request,
			LanguageId to,
			Fn<void(Ui::TranslateProviderResult)> done) override {
		requestBatch(
			{ std::move(request) },
			to,
			[done = std::move(done)](
					int,
					Ui::TranslateProviderResult result) {
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
		struct Group {
			TranslationJob job;
			std::vector<int> indices;
		};
		struct State {
			std::vector<MarkupMasked> masked;
			std::vector<TextWithEntities> sources;
			int remaining = 0;
			Fn<void(int, Ui::TranslateProviderResult)> doneOne;
			Fn<void()> doneAll;
		};
		const auto fail = [](Ui::TranslateProviderError error) {
			return Ui::TranslateProviderResult{ .error = error };
		};
		auto state = std::make_shared<State>();
		state->doneOne = std::move(doneOne);
		state->doneAll = std::move(doneAll);
		const auto weak = base::make_weak(this);
		const auto finish = [=](int index, Ui::TranslateProviderResult result) {
			if (!weak) {
				return;
			}
			state->doneOne(index, std::move(result));
			if (!--state->remaining) {
				state->doneAll();
			}
		};
		const auto target = to.twoLetterCode();
		const auto installed = InstalledTranslateModels();
		const auto candidates = Candidates(installed, target);
		auto groups = std::map<QString, Group>();
		state->remaining = int(requests.size());
		state->masked.resize(requests.size());
		state->sources.resize(requests.size());
		auto immediate = std::vector<std::pair<int, Ui::TranslateProviderResult>>();
		for (auto i = 0; i != int(requests.size()); ++i) {
			auto &source = state->sources[i];
			source = std::move(requests[i].text);
			const auto from = DetectLanguage(source.text, candidates);
			if (!from.isEmpty() && from == target) {
				immediate.emplace_back(
					i,
					Ui::TranslateProviderResult{ .text = source });
				continue;
			}
			const auto direction = ChooseDirection(installed, from, target);
			if (direction.missing) {
				immediate.emplace_back(
					i,
					fail(Ui::TranslateProviderError::LocalLanguagePackMissing));
				continue;
			}
			state->masked[i] = MaskMarkup(ToMarkup(source), RoleOf);
			auto key = QString();
			for (const auto &model : direction.chain) {
				key += model.code + ';';
			}
			auto &group = groups[key];
			if (group.job.chain.empty()) {
				for (const auto &model : direction.chain) {
					group.job.chain.push_back({
						.path = ModelPath(model),
						.sha256 = model.sha256,
					});
				}
			}
			group.job.inputs.push_back({
				.text = state->masked[i].text,
				.spans = state->masked[i].spans,
			});
			group.indices.push_back(i);
		}
		for (auto &entry : immediate) {
			crl::on_main(weak, [
				=,
				index = entry.first,
				result = std::move(entry.second)
			]() mutable {
				finish(index, std::move(result));
			});
		}
		for (auto &entry : groups) {
			auto &group = entry.second;
			auto flag = TranslateTextsAsync(
				std::move(group.job),
				[=, indices = std::move(group.indices)](
						TranslationResult result) {
					if (!weak) {
						return;
					}
					for (auto k = 0; k != int(indices.size()); ++k) {
						const auto index = indices[k];
						if (!result.ok || k >= int(result.outputs.size())) {
							finish(
								index,
								fail(Ui::TranslateProviderError::Unknown));
							continue;
						}
						const auto &masked = state->masked[index];
						const auto &output = result.outputs[k];
						const auto restored = RestoreMarkup(
							masked,
							state->sources[index].text,
							output.text,
							output.spans);
						auto text = FromMarkup(restored);
						KeepLostLinks(text, masked, restored);
						finish(index, Ui::TranslateProviderResult{
							.text = std::move(text),
						});
					}
				});
			_flags.push_back(std::move(flag));
		}
	}

private:
	std::vector<std::shared_ptr<std::atomic<bool>>> _flags;

};

} // namespace

std::unique_ptr<Ui::TranslateProvider> CreateLocalTranslateProvider() {
	return std::make_unique<Provider>();
}

} // namespace Ayu::Translator
