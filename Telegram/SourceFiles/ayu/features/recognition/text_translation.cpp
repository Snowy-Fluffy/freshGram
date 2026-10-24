// This is the source code of freshGram.
#include "ayu/features/recognition/text_translation.h"

#include "ayu/features/recognition/recognition_models.h"
#include "base/timer.h"
#include "logs.h"

#ifdef FRESHGRAM_LOCAL_TRANSLATE
#include <ctranslate2/logging.h>
#include <ctranslate2/models/model_reader.h>
#include <ctranslate2/translator.h>
#include <sentencepiece_processor.h>
#endif // FRESHGRAM_LOCAL_TRANSLATE

#include <QtCore/QDataStream>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QThread>

#include <algorithm>
#include <map>
#include <mutex>

namespace FreshRecognition {
namespace {

constexpr auto kSoftSentence = 200;
constexpr auto kMaxSentence = 300;
constexpr auto kHardSentence = 450;

[[nodiscard]] bool IsTerminator(QChar c) {
	switch (c.unicode()) {
	case '.':
	case '!':
	case '?':
	case 0x2026:
	case 0x203C:
	case 0x2047:
	case 0x2048:
	case 0x2049:
	case 0x0589:
	case 0x061F:
	case 0x06D4:
	case 0x0964:
	case 0x0965:
	case 0x1362:
	case 0x3002:
	case 0xFF01:
	case 0xFF0E:
	case 0xFF1F:
	case 0xFF61:
		return true;
	}
	return false;
}

[[nodiscard]] bool IsWideTerminator(QChar c) {
	return (c.unicode() == 0x3002)
		|| (c.unicode() == 0xFF01)
		|| (c.unicode() == 0xFF0E)
		|| (c.unicode() == 0xFF1F)
		|| (c.unicode() == 0xFF61);
}

[[nodiscard]] bool IsCloser(QChar c) {
	switch (c.unicode()) {
	case '"':
	case '\'':
	case ')':
	case ']':
	case '}':
	case 0xBB:
	case 0x2019:
	case 0x201D:
	case 0x300D:
		return true;
	}
	return false;
}

[[nodiscard]] bool IsClauseBreak(QChar c) {
	switch (c.unicode()) {
	case ',':
	case ';':
	case ':':
	case 0x3001:
	case 0xFF0C:
	case 0xFF1B:
		return true;
	}
	return false;
}

[[nodiscard]] bool IsLineBreak(QChar c) {
	return (c == '\n')
		|| (c == '\r')
		|| (c == QChar::LineSeparator)
		|| (c == QChar::ParagraphSeparator);
}

// A lone letter is not worth translating, the model makes things up.
[[nodiscard]] bool HasLetters(const QString &text) {
	return std::count_if(text.begin(), text.end(), [](QChar c) {
		return c.isLetter();
	}) >= 2;
}

} // namespace

std::vector<TextPart> SplitForTranslation(const QString &text) {
	auto result = std::vector<TextPart>();
	const auto size = int(text.size());
	auto i = 0;
	while (i < size) {
		auto from = i;
		while (i < size && text[i].isSpace()) {
			++i;
		}
		if (i > from) {
			result.push_back({ text.mid(from, i - from), false });
		}
		if (i >= size) {
			break;
		}
		from = i;
		while (i < size) {
			const auto c = text[i++];
			if (IsLineBreak(c)) {
				--i;
				break;
			} else if (IsTerminator(c)) {
				while (i < size && (IsTerminator(text[i]) || IsCloser(text[i]))) {
					++i;
				}
				if (i >= size || text[i].isSpace() || IsWideTerminator(c)) {
					break;
				}
			} else if ((i - from) >= kHardSentence) {
				if (i < size && c.isHighSurrogate() && text[i].isLowSurrogate()) {
					++i;
				}
				break;
			} else if ((i - from) >= kMaxSentence && c.isSpace()) {
				break;
			} else if ((i - from) >= kSoftSentence && IsClauseBreak(c)) {
				break;
			}
		}
		auto till = i;
		while (till > from && text[till - 1].isSpace()) {
			--till;
		}
		i = till;
		const auto sentence = text.mid(from, till - from);
		result.push_back({ sentence, HasLetters(sentence) });
	}
	return result;
}

#ifdef FRESHGRAM_LOCAL_TRANSLATE

namespace {

constexpr auto kContainerMagic = "FGTMODEL";
constexpr auto kMagicSize = 8;
constexpr auto kMaxSections = 16;
constexpr auto kMaxNameSize = 64;
constexpr auto kMaxSectionSize = qint64(400) * 1024 * 1024;
constexpr auto kMaxLoaded = 2;
constexpr auto kIdleRelease = 3 * 60 * crl::time(1000);

struct Section {
	qint64 offset = 0;
	qint64 size = 0;
};

using Sections = std::map<QString, Section>;

[[nodiscard]] bool IsKnownSection(const QString &name) {
	static const auto names = QStringList{
		u"model.bin"_q,
		u"config.json"_q,
		u"shared_vocabulary.json"_q,
		u"shared_vocabulary.txt"_q,
		u"source_vocabulary.json"_q,
		u"source_vocabulary.txt"_q,
		u"target_vocabulary.json"_q,
		u"target_vocabulary.txt"_q,
		u"source.spm"_q,
		u"target.spm"_q,
		u"prefix.txt"_q,
	};
	return names.contains(name);
}

// Magic, a count, then the name and the size of each file, then the data.
[[nodiscard]] std::optional<Sections> ReadSections(QFile &file) {
	auto magic = QByteArray(kMagicSize, Qt::Uninitialized);
	if (file.read(magic.data(), kMagicSize) != kMagicSize
		|| magic != QByteArray(kContainerMagic, kMagicSize)) {
		return std::nullopt;
	}
	auto stream = QDataStream(&file);
	stream.setByteOrder(QDataStream::LittleEndian);
	auto count = quint32();
	stream >> count;
	if (stream.status() != QDataStream::Ok
		|| !count
		|| count > kMaxSections) {
		return std::nullopt;
	}
	auto names = std::vector<std::pair<QString, qint64>>();
	for (auto i = quint32(); i != count; ++i) {
		auto length = quint16();
		stream >> length;
		if (stream.status() != QDataStream::Ok
			|| !length
			|| length > kMaxNameSize) {
			return std::nullopt;
		}
		auto name = QByteArray(length, Qt::Uninitialized);
		auto size = quint64();
		if (stream.readRawData(name.data(), length) != length) {
			return std::nullopt;
		}
		stream >> size;
		if (stream.status() != QDataStream::Ok
			|| size > quint64(kMaxSectionSize)) {
			return std::nullopt;
		}
		names.emplace_back(QString::fromLatin1(name), qint64(size));
	}
	auto result = Sections();
	auto offset = file.pos();
	for (const auto &[name, size] : names) {
		if (!IsKnownSection(name) || result.contains(name)) {
			return std::nullopt;
		}
		result.emplace(name, Section{ offset, size });
		offset += size;
	}
	if (offset != file.size()) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] std::optional<std::string> ReadSection(
		QFile &file,
		const Sections &sections,
		const QString &name) {
	const auto i = sections.find(name);
	if (i == end(sections)) {
		return std::nullopt;
	}
	auto result = std::string(size_t(i->second.size), '\0');
	if (!file.seek(i->second.offset)
		|| file.read(result.data(), i->second.size) != i->second.size) {
		return std::nullopt;
	}
	return result;
}

class ViewBuffer final : public std::streambuf {
public:
	explicit ViewBuffer(const std::string &data) {
		const auto begin = const_cast<char*>(data.data());
		setg(begin, begin, begin + data.size());
	}

protected:
	pos_type seekoff(
			off_type offset,
			std::ios_base::seekdir direction,
			std::ios_base::openmode which) override {
		if (!(which & std::ios_base::in)) {
			return pos_type(off_type(-1));
		}
		auto target = offset;
		if (direction == std::ios_base::cur) {
			target += (gptr() - eback());
		} else if (direction == std::ios_base::end) {
			target += (egptr() - eback());
		}
		if (target < 0 || target > (egptr() - eback())) {
			return pos_type(off_type(-1));
		}
		setg(eback(), eback() + target, egptr());
		return pos_type(target);
	}

	pos_type seekpos(
			pos_type position,
			std::ios_base::openmode which) override {
		return seekoff(off_type(position), std::ios_base::beg, which);
	}

};

class ViewStream final : public std::istream {
public:
	explicit ViewStream(const std::string &data)
	: std::istream(nullptr)
	, _buffer(data) {
		rdbuf(&_buffer);
	}

private:
	ViewBuffer _buffer;

};

class ContainerReader final : public ctranslate2::models::ModelReader {
public:
	explicit ContainerReader(std::string id) : _id(std::move(id)) {
	}

	void add(std::string name, std::string content) {
		_files.emplace(std::move(name), std::move(content));
	}

	std::string get_model_id() const override {
		return _id;
	}

	std::unique_ptr<std::istream> get_file(
			const std::string &filename,
			const bool binary) override {
		const auto i = _files.find(filename);
		return (i == end(_files))
			? nullptr
			: std::make_unique<ViewStream>(i->second);
	}

private:
	std::string _id;
	std::map<std::string, std::string> _files;

};

struct Loaded {
	QString key;
	// The target language token that some models need before a sentence.
	std::string prefix;
	std::unique_ptr<ctranslate2::Translator> translator;
	sentencepiece::SentencePieceProcessor source;
	sentencepiece::SentencePieceProcessor target;
};

std::mutex CacheMutex;
std::vector<std::shared_ptr<Loaded>> Cache;
std::unique_ptr<base::Timer> IdleTimer;

void ReleaseModels() {
	const auto lock = std::lock_guard(CacheMutex);
	Cache.clear();
}

void ScheduleRelease() {
	if (!IdleTimer) {
		IdleTimer = std::make_unique<base::Timer>(ReleaseModels);
	}
	IdleTimer->callOnce(kIdleRelease);
}

[[nodiscard]] std::shared_ptr<Loaded> Load(
		const TranslationModel &model,
		QString &error) {
	const auto key = model.path + '|' + model.sha256;
	const auto lock = std::lock_guard(CacheMutex);
	if (!QFileInfo::exists(model.path)) {
		Cache.erase(std::remove_if(begin(Cache), end(Cache), [&](auto &loaded) {
			return loaded->key.startsWith(model.path + '|');
		}), end(Cache));
		error = u"The model is not downloaded."_q;
		return nullptr;
	}
	for (auto i = begin(Cache); i != end(Cache); ++i) {
		if ((*i)->key == key) {
			auto result = *i;
			std::rotate(i, i + 1, end(Cache));
			return result;
		}
	}
	if (!ModelFileIntact(model.path, model.sha256)) {
		error = u"The model failed the integrity check."_q;
		return nullptr;
	}
	auto file = QFile(model.path);
	if (!file.open(QIODevice::ReadOnly)) {
		error = u"Could not open the model."_q;
		return nullptr;
	}
	const auto sections = ReadSections(file);
	if (!sections) {
		error = u"The model file is damaged."_q;
		return nullptr;
	}
	auto result = std::make_shared<Loaded>();
	result->key = key;
	auto spmSource = ReadSection(file, *sections, u"source.spm"_q);
	auto spmTarget = ReadSection(file, *sections, u"target.spm"_q);
	if (!spmSource || !spmTarget
		|| !result->source.LoadFromSerializedProto(*spmSource).ok()
		|| !result->target.LoadFromSerializedProto(*spmTarget).ok()) {
		error = u"Could not load the tokenizer."_q;
		return nullptr;
	}
	auto reader = std::make_shared<ContainerReader>(
		model.path.toStdString());
	if (auto prefix = ReadSection(file, *sections, u"prefix.txt"_q)) {
		result->prefix = QString::fromStdString(*prefix).trimmed().toStdString();
	}
	for (const auto &[name, section] : *sections) {
		if (name == u"source.spm"_q
			|| name == u"target.spm"_q
			|| name == u"prefix.txt"_q) {
			continue;
		}
		auto content = ReadSection(file, *sections, name);
		if (!content) {
			error = u"The model file is damaged."_q;
			return nullptr;
		}
		reader->add(name.toStdString(), std::move(*content));
	}
	try {
		ctranslate2::set_log_level(ctranslate2::LogLevel::Off);
		auto loader = ctranslate2::models::ModelLoader(reader);
		loader.device = ctranslate2::Device::CPU;
		loader.compute_type = ctranslate2::ComputeType::DEFAULT;
		result->translator = std::make_unique<ctranslate2::Translator>(
			loader,
			ctranslate2::ReplicaPoolConfig{
				.num_threads_per_replica = size_t(
					std::clamp(QThread::idealThreadCount() / 2, 1, 4)),
			});
	} catch (const std::exception &e) {
		LOG(("Translation Error: could not load the model: %1"
			).arg(QString::fromUtf8(e.what())));
		error = u"Could not load the model."_q;
		return nullptr;
	}
	Cache.push_back(result);
	if (Cache.size() > kMaxLoaded) {
		Cache.erase(begin(Cache));
	}
	return result;
}

// Where each UTF-16 index of a text starts in its UTF-8 form.
[[nodiscard]] std::vector<int> ByteOffsets(const QString &text) {
	auto result = std::vector<int>(text.size() + 1, 0);
	auto bytes = 0;
	for (auto i = 0; i < text.size();) {
		result[i] = bytes;
		const auto c = text[i];
		if (c.isHighSurrogate()
			&& (i + 1) < text.size()
			&& text[i + 1].isLowSurrogate()) {
			result[i + 1] = bytes;
			bytes += 4;
			i += 2;
		} else {
			const auto code = c.unicode();
			bytes += (code < 0x80) ? 1 : (code < 0x800) ? 2 : 3;
			++i;
		}
	}
	result[text.size()] = bytes;
	return result;
}

[[nodiscard]] int IndexAt(const std::vector<int> &offsets, int bytes) {
	const auto i = std::lower_bound(begin(offsets), end(offsets), bytes);
	return int(i - begin(offsets));
}

[[nodiscard]] bool IsWordChar(QChar c) {
	return c.isLetterOrNumber() || c.isMark();
}

constexpr auto kMinConfidence = 0.3f;
constexpr auto kClusterGap = 2;
constexpr auto kBatch = size_t(16);

struct PartSpan {
	int index = 0;
	int begin = 0;
	int end = 0;
	bool full = false;
	bool lost = false;
};

struct Unit {
	bool translate = false;
	int start = 0;
	QString text;
	std::string current;
	std::vector<PartSpan> spans;
};

struct TargetWord {
	int first = 0;
	int last = 0;
	// The first and the last pieces that have letters or digits.
	int from = -1;
	int till = -1;
};

[[nodiscard]] bool HasWordChar(const std::string &utf8) {
	const auto text = QString::fromStdString(utf8);
	return std::any_of(text.begin(), text.end(), [](QChar c) {
		return c.isLetterOrNumber();
	});
}

// Scripts that are written without spaces between sentences.
[[nodiscard]] bool IsUnspaced(QChar c) {
	const auto code = c.unicode();
	return (code >= 0x2E80 && code <= 0x9FFF)
		|| (code >= 0xAC00 && code <= 0xD7AF)
		|| (code >= 0xF900 && code <= 0xFAFF)
		|| (code >= 0xFF00 && code <= 0xFFEF);
}

// The model sometimes loops, then the sentence is better left as it is.
[[nodiscard]] bool IsDegenerate(const std::vector<std::string> &tokens) {
	const auto size = int(tokens.size());
	for (auto n = 1; n <= 4; ++n) {
		for (auto start = 0; start + n <= size; ++start) {
			auto repeats = 1;
			for (auto next = start + n; next + n <= size; next += n) {
				if (!std::equal(
						begin(tokens) + start,
						begin(tokens) + start + n,
						begin(tokens) + next)) {
					break;
				}
				++repeats;
			}
			auto words = false;
			for (auto k = start; k != start + n; ++k) {
				words = words || HasWordChar(tokens[k]);
			}
			if (repeats >= ((n == 1) ? (words ? 4 : 2) : 3)) {
				return true;
			}
		}
	}
	return false;
}

void MapSpans(
		Unit &unit,
		int shift,
		const sentencepiece::ImmutableSentencePieceText &source,
		const std::vector<std::vector<float>> &attention,
		const sentencepiece::ImmutableSentencePieceText &target) {
	const auto count = int(source.pieces_size());
	const auto steps = std::min(
		int(attention.size()),
		int(target.pieces_size()));
	auto aligned = std::vector<int>(steps, -1);
	auto weight = std::vector<float>(steps, 0.f);
	for (auto t = 0; t != steps; ++t) {
		const auto &row = attention[t];
		auto best = -1;
		auto top = 0.f;
		for (auto s = 0; s != int(row.size()); ++s) {
			if (row[s] > top) {
				top = row[s];
				best = s;
			}
		}
		if (best >= shift && (best - shift) < count) {
			aligned[t] = best - shift;
			weight[t] = top;
		}
	}
	auto words = std::vector<TargetWord>();
	for (auto t = 0; t != steps; ++t) {
		const auto piece = target.pieces(t);
		if (piece.end() <= piece.begin()) {
			continue;
		}
		const auto starts = piece.piece().rfind("\xE2\x96\x81", 0) == 0;
		if (words.empty() || starts) {
			words.push_back({ .first = t, .last = t });
		} else {
			words.back().last = t;
		}
		if (HasWordChar(piece.surface())) {
			if (words.back().from < 0) {
				words.back().from = t;
			}
			words.back().till = t;
		}
	}
	const auto targetSize = int(target.text().size());
	for (auto &span : unit.spans) {
		if (span.lost) {
			continue;
		} else if (span.full) {
			span.begin = 0;
			span.end = targetSize;
			continue;
		}
		auto inside = std::vector<bool>(count, false);
		auto styled = false;
		for (auto s = 0; s != count; ++s) {
			const auto piece = source.pieces(s);
			inside[s] = (int(piece.begin()) < span.end)
				&& (int(piece.end()) > span.begin);
			styled = styled || (inside[s] && HasWordChar(piece.surface()));
		}
		auto chosen = std::vector<int>();
		auto current = std::vector<int>();
		const auto flush = [&] {
			if (current.size() > chosen.size()) {
				chosen = current;
			}
			current.clear();
		};
		for (auto w = 0; w != int(words.size()); ++w) {
			const auto &word = words[w];
			const auto text = (word.from >= 0);
			if (styled && !text) {
				continue;
			}
			const auto first = text ? word.from : word.first;
			const auto last = text ? word.till : word.last;
			auto hits = 0;
			auto total = 0;
			for (auto t = first; t != last + 1; ++t) {
				if (text && !HasWordChar(target.pieces(t).surface())) {
					continue;
				}
				++total;
				hits += (aligned[t] >= 0 && inside[aligned[t]]) ? 1 : 0;
			}
			if (hits * 2 <= total) {
				continue;
			}
			if (!current.empty() && (w - current.back()) > kClusterGap) {
				flush();
			}
			current.push_back(w);
		}
		flush();
		auto sum = 0.f;
		auto aligns = 0;
		for (const auto w : chosen) {
			for (auto t = words[w].first; t != words[w].last + 1; ++t) {
				if (aligned[t] >= 0 && inside[aligned[t]]) {
					sum += weight[t];
					++aligns;
				}
			}
		}
		if (chosen.empty() || (sum / aligns) < kMinConfidence) {
			span.lost = true;
			continue;
		}
		const auto &front = words[chosen.front()];
		const auto &back = words[chosen.back()];
		span.begin = int(target.pieces(
			(front.from >= 0) ? front.from : front.first).begin());
		span.end = int(target.pieces(
			(back.till >= 0) ? back.till : back.last).end());
	}
}

[[nodiscard]] bool RunHop(
		Loaded &loaded,
		const std::vector<Unit*> &units,
		const std::atomic<bool> &cancelled) {
	auto indices = std::vector<size_t>();
	auto sources = std::vector<std::vector<std::string>>();
	auto encoded = std::vector<sentencepiece::ImmutableSentencePieceText>();
	auto longest = size_t();
	for (auto i = size_t(); i != units.size(); ++i) {
		auto source = loaded.source.EncodeAsImmutableProto(units[i]->current);
		if (!source.pieces_size()) {
			continue;
		}
		auto tokens = std::vector<std::string>();
		if (!loaded.prefix.empty()) {
			tokens.push_back(loaded.prefix);
		}
		for (auto k = size_t(); k != source.pieces_size(); ++k) {
			tokens.push_back(source.pieces(int(k)).piece());
		}
		tokens.push_back("</s>");
		longest = std::max(longest, tokens.size());
		indices.push_back(i);
		sources.push_back(std::move(tokens));
		encoded.push_back(std::move(source));
	}
	if (sources.empty()) {
		return true;
	}
	auto options = ctranslate2::TranslationOptions();
	options.beam_size = 4;
	options.max_decoding_length = std::min<size_t>(512, longest * 3 + 16);
	options.return_attention = true;
	auto results = std::vector<ctranslate2::TranslationResult>();
	results.reserve(sources.size());
	for (auto from = size_t(); from < sources.size(); from += kBatch) {
		if (cancelled.load()) {
			return false;
		}
		const auto till = std::min(from + kBatch, sources.size());
		const auto slice = std::vector<std::vector<std::string>>(
			begin(sources) + from,
			begin(sources) + till);
		try {
			auto part = loaded.translator->translate_batch(
				slice,
				options,
				kBatch);
			for (auto &result : part) {
				results.push_back(std::move(result));
			}
		} catch (const std::exception &e) {
			LOG(("Translation Error: %1").arg(QString::fromUtf8(e.what())));
			for (const auto &tokens : slice) {
				try {
					auto one = loaded.translator->translate_batch(
						{ tokens },
						options);
					results.push_back(std::move(one.front()));
				} catch (const std::exception &) {
					results.emplace_back();
				}
			}
		}
	}
	const auto none = std::vector<std::vector<float>>();
	for (auto k = size_t(); k != indices.size(); ++k) {
		if (results[k].hypotheses.empty()) {
			continue;
		}
		const auto &output = results[k].output();
		if (output.empty()
			|| output.size() > sources[k].size() * 4 + 16
			|| IsDegenerate(output)) {
			continue;
		}
		const auto target = loaded.target.DecodePiecesAsImmutableProto(
			output);
		if (target.text().empty()) {
			continue;
		}
		const auto &unit = units[indices[k]];
		MapSpans(
			*unit,
			loaded.prefix.empty() ? 0 : 1,
			encoded[k],
			results[k].attention.empty() ? none : results[k].attention[0],
			target);
		unit->current = target.text();
	}
	return true;
}

[[nodiscard]] TextSpan Finish(
		const Unit &unit,
		const PartSpan &span) {
	if (!unit.translate) {
		return { span.begin, span.end - span.begin };
	}
	const auto &text = unit.text;
	const auto offsets = ByteOffsets(text);
	auto begin = IndexAt(offsets, span.begin);
	auto end = IndexAt(offsets, span.end);
	while (begin > 0
		&& begin < text.size()
		&& IsWordChar(text[begin - 1])
		&& IsWordChar(text[begin])) {
		--begin;
	}
	while (end > 0
		&& end < text.size()
		&& IsWordChar(text[end - 1])
		&& IsWordChar(text[end])) {
		++end;
	}
	return { begin, std::max(end - begin, 0) };
}

[[nodiscard]] TranslationResult Run(
		const TranslationJob &job,
		const std::atomic<bool> &cancelled) {
	auto result = TranslationResult();
	if (job.chain.empty()) {
		result.error = u"No model."_q;
		return result;
	}
	auto models = std::vector<std::shared_ptr<Loaded>>();
	for (const auto &model : job.chain) {
		auto loaded = Load(model, result.error);
		if (!loaded) {
			return result;
		}
		models.push_back(std::move(loaded));
	}
	auto texts = std::vector<std::vector<Unit>>(job.inputs.size());
	auto active = std::vector<Unit*>();
	for (auto i = size_t(); i != job.inputs.size(); ++i) {
		const auto &input = job.inputs[i];
		auto start = 0;
		for (auto &part : SplitForTranslation(input.text)) {
			auto unit = Unit();
			unit.translate = part.translate;
			unit.start = start;
			unit.text = part.text;
			start += int(part.text.size());
			texts[i].push_back(std::move(unit));
		}
		for (auto &unit : texts[i]) {
			const auto from = unit.start;
			const auto till = unit.start + int(unit.text.size());
			const auto offsets = unit.translate
				? ByteOffsets(unit.text)
				: std::vector<int>();
			for (auto k = 0; k != int(input.spans.size()); ++k) {
				const auto &span = input.spans[k];
				const auto spanFrom = std::max(span.offset, from);
				const auto spanTill = std::min(
					span.offset + span.length,
					till);
				if (span.offset < 0
					|| span.length <= 0
					|| span.offset + span.length > input.text.size()
					|| spanTill <= spanFrom) {
					continue;
				}
				auto part = PartSpan{ .index = k };
				const auto localFrom = spanFrom - from;
				const auto localTill = spanTill - from;
				part.full = (localFrom == 0)
					&& (localTill == unit.text.size());
				part.begin = unit.translate ? offsets[localFrom] : localFrom;
				part.end = unit.translate ? offsets[localTill] : localTill;
				unit.spans.push_back(part);
			}
		}
	}
	for (auto &list : texts) {
		for (auto &unit : list) {
			if (unit.translate) {
				unit.current = unit.text.toStdString();
				active.push_back(&unit);
			}
		}
	}
	try {
		for (const auto &loaded : models) {
			if (cancelled.load()) {
				result.cancelled = true;
				return result;
			}
			if (!RunHop(*loaded, active, cancelled)) {
				result.cancelled = true;
				return result;
			}
		}
	} catch (const std::exception &e) {
		LOG(("Translation Error: %1").arg(QString::fromUtf8(e.what())));
		result.error = u"The translation failed."_q;
		return result;
	}
	for (auto &unit : active) {
		unit->text = QString::fromStdString(unit->current);
	}
	for (auto i = size_t(); i != texts.size(); ++i) {
		auto output = TranslationOutput();
		auto from = std::vector<int>(job.inputs[i].spans.size(), -1);
		auto till = std::vector<int>(job.inputs[i].spans.size(), -1);
		const Unit *previous = nullptr;
		for (const auto &unit : texts[i]) {
			if (previous
				&& previous->translate
				&& unit.translate
				&& !output.text.isEmpty()
				&& !unit.text.isEmpty()
				&& !output.text.back().isSpace()
				&& !unit.text.front().isSpace()
				&& !IsUnspaced(output.text.back())
				&& !IsUnspaced(unit.text.front())) {
				output.text += ' ';
			}
			previous = &unit;
			const auto base = int(output.text.size());
			for (const auto &span : unit.spans) {
				if (span.lost) {
					continue;
				}
				const auto range = Finish(unit, span);
				if (range.length <= 0) {
					continue;
				}
				const auto a = base + range.offset;
				const auto b = a + range.length;
				from[span.index] = (from[span.index] < 0)
					? a
					: std::min(from[span.index], a);
				till[span.index] = std::max(till[span.index], b);
			}
			output.text += unit.text;
		}
		output.spans.resize(from.size());
		for (auto k = size_t(); k != from.size(); ++k) {
			if (from[k] >= 0 && till[k] > from[k]) {
				output.spans[k] = { from[k], till[k] - from[k] };
			}
		}
		result.outputs.push_back(std::move(output));
	}
	result.ok = true;
	return result;
}

} // namespace

#endif // FRESHGRAM_LOCAL_TRANSLATE

bool TextTranslationAvailable() {
#ifdef FRESHGRAM_LOCAL_TRANSLATE
	return true;
#else // FRESHGRAM_LOCAL_TRANSLATE
	return false;
#endif // !FRESHGRAM_LOCAL_TRANSLATE
}

std::shared_ptr<std::atomic<bool>> TranslateTextsAsync(
		TranslationJob job,
		Fn<void(TranslationResult)> done) {
	auto cancelled = std::make_shared<std::atomic<bool>>(false);
#ifdef FRESHGRAM_LOCAL_TRANSLATE
	crl::async([=] {
		auto result = Run(job, *cancelled);
		crl::on_main([=, result = std::move(result)]() mutable {
			ScheduleRelease();
			done(std::move(result));
		});
	});
#else // FRESHGRAM_LOCAL_TRANSLATE
	crl::on_main([=] {
		auto result = TranslationResult();
		result.error = u"Not available in this build."_q;
		done(std::move(result));
	});
#endif // !FRESHGRAM_LOCAL_TRANSLATE
	return cancelled;
}

} // namespace FreshRecognition
