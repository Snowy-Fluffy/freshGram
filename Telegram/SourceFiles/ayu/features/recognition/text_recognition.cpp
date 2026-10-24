// This is the source code of freshGram.
#include "ayu/features/recognition/text_recognition.h"

#include "ayu/features/recognition/recognition_models.h"
#include "logs.h"

#ifdef FRESHGRAM_LOCAL_OCR
#include <tesseract/baseapi.h>
#include <tesseract/ocrclass.h>
#endif // FRESHGRAM_LOCAL_OCR

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStringList>
#include <QtGui/QPainter>

#include <algorithm>
#include <initializer_list>
#include <mutex>

namespace FreshRecognition {
namespace {

#ifdef FRESHGRAM_LOCAL_OCR

constexpr auto kMaxLanguages = 3;
constexpr auto kPadding = 24;
constexpr auto kMaxSide = 6000;
constexpr auto kMaxInputSide = 4500;

#ifdef Q_OS_WIN
constexpr auto kNullDevice = "NUL";
#else // Q_OS_WIN
constexpr auto kNullDevice = "/dev/null";
#endif // !Q_OS_WIN

// WHY: Tesseract keeps some parameters, like debug_file, in globals.
std::mutex EngineMutex;

thread_local const QHash<QString, QString> *ExpectedHashes = nullptr;
thread_local bool IntegrityFailed = false;

// Opens the files through Qt, so non-ASCII paths work on Windows.
bool ReadFile(const char *filename, std::vector<char> *data) {
	auto file = QFile(QString::fromUtf8(filename));
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	const auto bytes = file.readAll();
	const auto name = QFileInfo(file).fileName();
	const auto expected = ExpectedHashes
		? ExpectedHashes->value(name)
		: QString();
	if (expected.isEmpty()
		|| QCryptographicHash::hash(
			bytes,
			QCryptographicHash::Sha256).toHex() != expected.toLatin1()) {
		LOG(("Recognition Error: '%1' failed the integrity check."
			).arg(name));
		IntegrityFailed = true;
		return false;
	}
	data->assign(bytes.constData(), bytes.constData() + bytes.size());
	return true;
}

bool Cancelled(void *flag, int) {
	return static_cast<const std::atomic<bool>*>(flag)->load();
}

// Gray, upscaled when small, light text on a dark background inverted,
// with a white margin.
[[nodiscard]] QImage Prepare(const QImage &source) {
	auto flat = QImage(source.size(), QImage::Format_RGB32);
	flat.fill(Qt::white);
	{
		auto p = QPainter(&flat);
		p.drawImage(QRect(QPoint(), source.size()), source);
	}
	auto gray = flat.convertToFormat(QImage::Format_Grayscale8);

	const auto longest = std::max(gray.width(), gray.height());
	if (longest > kMaxInputSide) {
		gray = gray.scaled(
			gray.size() * kMaxInputSide / longest,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
	}
	const auto factor = (longest <= 1000)
		? 3
		: (longest <= 2000)
		? 2
		: 1;
	if (factor > 1 && longest * factor <= kMaxSide) {
		gray = gray.scaled(
			gray.size() * factor,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
	}

	auto sum = quint64(0);
	auto count = quint64(0);
	for (auto y = 0; y < gray.height(); y += 4) {
		const auto line = gray.constScanLine(y);
		for (auto x = 0; x < gray.width(); x += 4) {
			sum += line[x];
			++count;
		}
	}
	if (count && (sum / count) < 110) {
		gray.invertPixels();
	}

	auto result = QImage(
		gray.width() + 2 * kPadding,
		gray.height() + 2 * kPadding,
		QImage::Format_Grayscale8);
	result.fill(Qt::white);
	{
		auto p = QPainter(&result);
		p.drawImage(kPadding, kPadding, gray);
	}
	return result;
}

[[nodiscard]] QStringList LanguagesOfScript(const QString &script) {
	if (script == u"Cyrillic"_q) {
		return { u"rus"_q, u"ukr"_q };
	} else if (script == u"Latin"_q) {
		return {
			u"eng"_q, u"deu"_q, u"fra"_q, u"spa"_q,
			u"ita"_q, u"por"_q, u"pol"_q, u"tur"_q,
		};
	} else if (script == u"Arabic"_q) {
		return { u"ara"_q };
	} else if (script == u"Han"_q) {
		return { u"chi_sim"_q, u"chi_tra"_q, u"jpn"_q };
	} else if (script == u"Japanese"_q || script == u"Hiragana"_q
		|| script == u"Katakana"_q) {
		return { u"jpn"_q, u"chi_sim"_q, u"chi_tra"_q };
	} else if (script == u"Hangul"_q || script == u"Korean"_q) {
		return { u"kor"_q };
	} else if (script == u"Devanagari"_q) {
		return { u"hin"_q };
	}
	return {};
}

// Picks the installed languages for the detected script, English is
// always added as a companion.
[[nodiscard]] QString ChooseLanguages(
		const QString &script,
		const QStringList &installed) {
	const auto english = u"eng"_q;
	const auto hasEnglish = installed.contains(english);
	const auto limit = kMaxLanguages - (hasEnglish ? 1 : 0);
	auto chosen = QStringList();
	for (const auto &code : LanguagesOfScript(script)) {
		if (code != english
			&& installed.contains(code)
			&& !chosen.contains(code)) {
			chosen.push_back(code);
		}
	}
	if (chosen.isEmpty()) {
		for (const auto &code : installed) {
			if (code != english) {
				chosen.push_back(code);
			}
		}
	}
	while (chosen.size() > limit) {
		chosen.removeLast();
	}
	if (hasEnglish) {
		chosen.push_back(english);
	}
	return chosen.join('+');
}

[[nodiscard]] TextResult Recognize(
		const TextRequest &request,
		const std::atomic<bool> &cancelled) {
	auto result = TextResult();
	const auto lock = std::lock_guard(EngineMutex);
	if (cancelled.load()) {
		result.cancelled = true;
		return result;
	}
	ExpectedHashes = &request.hashes;
	IntegrityFailed = false;
	const auto &installed = request.installed;
	if (installed.isEmpty()) {
		result.error = RecognitionError::NoLanguages;
		return result;
	}
	const auto prepared = Prepare(request.image);
	const auto path = TessdataPath().toUtf8();
	const auto vars = std::vector<std::string>{ "debug_file" };
	const auto values = std::vector<std::string>{ kNullDevice };

	auto languages = request.languages;
	if (IsAutoOcr(languages)) {
		auto candidates = QStringList();
		for (const auto &code : OcrCodes(languages)) {
			if (installed.contains(code)) {
				candidates.push_back(code);
			}
		}
		if (candidates.isEmpty()) {
			candidates = installed;
		}
		auto script = QString();
		auto osd = tesseract::TessBaseAPI();
		if (osd.Init(
				path.constData(),
				0,
				"osd",
				tesseract::OEM_DEFAULT,
				nullptr,
				0,
				&vars,
				&values,
				false,
				&ReadFile) == 0) {
			osd.SetPageSegMode(tesseract::PSM_OSD_ONLY);
			osd.SetImage(
				prepared.constBits(),
				prepared.width(),
				prepared.height(),
				1,
				int(prepared.bytesPerLine()));
			int degrees = 0;
			float orientConfidence = 0.f;
			const char *name = nullptr;
			float scriptConfidence = 0.f;
			if (osd.DetectOrientationScript(
					&degrees,
					&orientConfidence,
					&name,
					&scriptConfidence)
				&& name) {
				script = QString::fromLatin1(name);
			}
			osd.End();
		}
		languages = ChooseLanguages(script, candidates);
	} else {
		auto chosen = QStringList();
		for (const auto &code : languages.split('+', Qt::SkipEmptyParts)) {
			if (installed.contains(code)) {
				chosen.push_back(code);
			}
		}
		languages = chosen.join('+');
	}
	if (cancelled.load()) {
		result.cancelled = true;
		return result;
	} else if (languages.isEmpty()) {
		result.error = RecognitionError::NoLanguages;
		return result;
	}

	auto api = tesseract::TessBaseAPI();
	const auto init = languages.toUtf8();
	if (api.Init(
			path.constData(),
			0,
			init.constData(),
			tesseract::OEM_LSTM_ONLY,
			nullptr,
			0,
			&vars,
			&values,
			false,
			&ReadFile) != 0) {
		result.error = IntegrityFailed
			? RecognitionError::Integrity
			: RecognitionError::LoadFailed;
		return result;
	}
	auto text = QString();
	for (const auto mode : {
		tesseract::PSM_AUTO,
		tesseract::PSM_SINGLE_BLOCK,
		tesseract::PSM_SPARSE_TEXT,
	}) {
		api.SetPageSegMode(mode);
		api.SetSourceResolution(300);
		api.SetImage(
			prepared.constBits(),
			prepared.width(),
			prepared.height(),
			1,
			int(prepared.bytesPerLine()));

		auto monitor = tesseract::ETEXT_DESC();
		monitor.cancel = &Cancelled;
		monitor.cancel_this = const_cast<std::atomic<bool>*>(&cancelled);
		const auto code = api.Recognize(&monitor);
		if (cancelled.load()) {
			result.cancelled = true;
			api.End();
			return result;
		} else if (code != 0) {
			result.error = RecognitionError::Failed;
			api.End();
			return result;
		}
		const auto raw = std::unique_ptr<char[]>(api.GetUTF8Text());
		text = raw ? QString::fromUtf8(raw.get()).trimmed() : QString();
		if (!text.isEmpty()) {
			break;
		}
	}
	result.text = text;
	result.languages = languages;
	result.ok = true;
	api.End();
	return result;
}

#endif // FRESHGRAM_LOCAL_OCR

} // namespace

bool TextRecognitionAvailable() {
#ifdef FRESHGRAM_LOCAL_OCR
	return true;
#else // FRESHGRAM_LOCAL_OCR
	return false;
#endif // !FRESHGRAM_LOCAL_OCR
}

std::shared_ptr<std::atomic<bool>> RecognizeTextAsync(
		TextRequest request,
		Fn<void(TextResult)> done) {
	auto cancelled = std::make_shared<std::atomic<bool>>(false);
#ifdef FRESHGRAM_LOCAL_OCR
	request.installed = InstalledOcrLanguages();
	request.hashes = TesseractHashes();
	crl::async([=] {
		const auto result = Recognize(request, *cancelled);
		crl::on_main([=] {
			done(result);
		});
	});
#else // FRESHGRAM_LOCAL_OCR
	crl::on_main([=] {
		auto result = TextResult();
		result.error = RecognitionError::Unavailable;
		done(result);
	});
#endif // !FRESHGRAM_LOCAL_OCR
	return cancelled;
}

} // namespace FreshRecognition
