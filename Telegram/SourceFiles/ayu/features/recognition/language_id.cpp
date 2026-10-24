// This is the source code of freshGram.
#include "ayu/features/recognition/language_id.h"

#ifdef FRESHGRAM_LOCAL_TRANSLATE
#include <fasttext.h>
#endif // FRESHGRAM_LOCAL_TRANSLATE

#include <QtCore/QFile>

#include <algorithm>
#include <memory>
#include <mutex>
#include <sstream>

namespace FreshRecognition {
namespace {

#ifdef FRESHGRAM_LOCAL_TRANSLATE

constexpr auto kMaxChars = 400;
constexpr auto kHintBoost = 0.6f;
constexpr auto kFileMagic = int32_t(793712314);

std::once_flag LoadFlag;
std::unique_ptr<fasttext::FastText> Model;

[[nodiscard]] const fasttext::FastText *Load() {
	std::call_once(LoadFlag, [] {
		auto file = QFile(u":/gui/fresh/lid.176.ftz"_q);
		if (!file.open(QIODevice::ReadOnly)) {
			return;
		}
		const auto bytes = file.readAll();
		auto stream = std::istringstream(
			std::string(bytes.constData(), size_t(bytes.size())),
			std::ios_base::in | std::ios_base::binary);
		auto model = std::make_unique<fasttext::FastText>();
		try {
			auto magic = int32_t();
			auto version = int32_t();
			stream.read(reinterpret_cast<char*>(&magic), sizeof(magic));
			stream.read(reinterpret_cast<char*>(&version), sizeof(version));
			if (!stream || magic != kFileMagic) {
				return;
			}
			model->loadModel(stream);
			Model = std::move(model);
		} catch (...) {
		}
	});
	return Model.get();
}

[[nodiscard]] bool HasAny(const QString &text, QStringView letters) {
	return std::any_of(text.begin(), text.end(), [&](QChar c) {
		return std::find(letters.begin(), letters.end(), c) != letters.end();
	});
}

void Boost(std::vector<LanguageGuess> &guesses, const QString &code) {
	auto i = std::find_if(begin(guesses), end(guesses), [&](const auto &g) {
		return g.code == code;
	});
	if (i == end(guesses)) {
		guesses.push_back({ code, 0.f });
		i = end(guesses) - 1;
	}
	i->probability = std::min(1.f, i->probability + kHintBoost);
}

#endif // FRESHGRAM_LOCAL_TRANSLATE

} // namespace

bool LanguageDetectionAvailable() {
#ifdef FRESHGRAM_LOCAL_TRANSLATE
	return true;
#else // FRESHGRAM_LOCAL_TRANSLATE
	return false;
#endif // !FRESHGRAM_LOCAL_TRANSLATE
}

std::vector<LanguageGuess> GuessLanguages(const QString &text, int count) {
	auto result = std::vector<LanguageGuess>();
#ifdef FRESHGRAM_LOCAL_TRANSLATE
	auto line = text.left(kMaxChars);
	line.replace('\n', ' ').replace('\r', ' ');
	if (std::count_if(line.begin(), line.end(), [](QChar c) {
		return c.isLetter();
	}) < 2) {
		return result;
	}
	const auto model = Load();
	if (!model) {
		return result;
	}
	auto predictions = std::vector<std::pair<fasttext::real, std::string>>();
	auto input = std::istringstream(line.toUtf8().toStdString());
	try {
		model->predictLine(input, predictions, count + 2, 0.f);
	} catch (...) {
		return result;
	}
	static const auto prefix = QString::fromLatin1("__label__");
	for (const auto &[probability, label] : predictions) {
		auto code = QString::fromStdString(label);
		if (code.startsWith(prefix)) {
			code = code.mid(prefix.size());
		}
		result.push_back({ code, probability });
	}
	const auto ukrainian = HasAny(line, u"іїєґІЇЄҐ");
	const auto russian = HasAny(line, u"ыэёЫЭЁ");
	if (ukrainian && !russian) {
		Boost(result, u"uk"_q);
	} else if (russian && !ukrainian) {
		Boost(result, u"ru"_q);
	}
	std::stable_sort(begin(result), end(result), [](
			const LanguageGuess &a,
			const LanguageGuess &b) {
		return a.probability > b.probability;
	});
	if (int(result.size()) > count) {
		result.resize(count);
	}
#endif // FRESHGRAM_LOCAL_TRANSLATE
	return result;
}

} // namespace FreshRecognition
