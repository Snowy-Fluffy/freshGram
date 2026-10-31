// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "codegen/lang/generator.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <functional>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSet>
#include <QtCore/QTextStream>
#include <QtGui/QImage>
#include <QtGui/QPainter>

namespace codegen {
namespace lang {
namespace {

constexpr int kErrorTooManyKeys = 841;
constexpr int kErrorTooManyTags = 842;

constexpr auto kIndexLimit = std::numeric_limits<ushort>::max();

struct Slot {
	int start = 0;
	int size = 0;
};

} // namespace

Generator::Generator(const LangPack &langpack, const QString &destBasePath, const common::ProjectInfo &project)
: langpack_(langpack)
, basePath_(destBasePath)
, baseName_(QFileInfo(basePath_).baseName())
, project_(project) {
	allocateIndices();
	saveTagOrder();
	collectDeclarations();
}

void Generator::saveTagOrder() {
	auto file = QFile(basePath_ + ".tags");
	if (file.open(QIODevice::WriteOnly)) {
		auto stream = QTextStream(&file);
		for (const auto &tag : langpack_.tags) {
			stream << tag.tag << '\n';
		}
	}
}

void Generator::allocateIndices() {
	const auto path = basePath_ + ".indices";
	auto known = std::map<QString, Slot>();
	auto next = 0;

	auto reading = QFile(path);
	if (reading.open(QIODevice::ReadOnly)) {
		auto stream = QTextStream(&reading);
		while (!stream.atEnd()) {
			const auto parts = stream.readLine().split('\t');
			if (parts.size() != 3) {
				continue;
			}
			const auto start = parts[1].toInt();
			const auto size = parts[2].toInt();
			if (size <= 0 || start < 0) {
				continue;
			}
			known.emplace(parts[0], Slot{ start, size });
			next = std::max(next, start + size);
		}
		reading.close();
	}

	indices_.assign(langpack_.entries.size(), -1);
	for (auto i = 0, count = int(langpack_.entries.size()); i != count; ++i) {
		const auto &entry = langpack_.entries[i];
		const auto isPlural = !entry.keyBase.isEmpty();
		if (isPlural && entry.key != ComputePluralKey(entry.keyBase, 0)) {
			continue; // Taken by the first part of the plural group.
		}
		const auto name = isPlural ? entry.keyBase : entry.key;
		const auto size = isPlural ? kPluralPartCount : 1;
		auto j = known.find(name);
		if (j == known.end() || j->second.size != size) {
			j = known.insert_or_assign(name, Slot{ next, size }).first;
			next += size;
		}
		for (auto shift = 0; shift != size; ++shift) {
			indices_[i + shift] = j->second.start + shift;
		}
	}
	keysCount_ = next;
	if (keysCount_ > kIndexLimit) {
		common::logError(kErrorTooManyKeys, path)
			<< "the key table needs " << keysCount_
			<< " slots, the limit is " << kIndexLimit
			<< ". Removed keys keep their slot so that re-adding one is free; "
			<< "delete this file to renumber the keys from scratch.";
		failed_ = true;
		return;
	}

	auto writing = QFile(path);
	if (writing.open(QIODevice::WriteOnly)) {
		auto stream = QTextStream(&writing);
		for (const auto &[name, slot] : known) {
			stream << name << '\t' << slot.start << '\t' << slot.size << '\n';
		}
	}
}

void Generator::collectDeclarations() {
	auto byIndex = std::map<int, QString>();
	for (auto i = 0, count = int(langpack_.entries.size()); i != count; ++i) {
		const auto &entry = langpack_.entries[i];
		const auto isPlural = !entry.keyBase.isEmpty();
		const auto &key = entry.key;
		if (isPlural && key != ComputePluralKey(entry.keyBase, 0)) {
			continue;
		}
		auto tags = QStringList();
		for (auto &tagData : entry.tags) {
			tags.push_back("lngtag_" + tagData.tag);
		}
		byIndex.emplace(indices_[i], "inline constexpr phrase<"
			+ tags.join(", ")
			+ "> "
			+ (isPlural ? entry.keyBase : key)
			+ "{ ushort("
			+ QString::number(indices_[i])
			+ ") };");
	}
	declarations_.reserve(byIndex.size());
	for (auto &[index, declaration] : byIndex) {
		declarations_.push_back(declaration);
	}
}

bool Generator::writeHeader() {
	if (failed_) {
		return false;
	} else if (int(langpack_.tags.size()) > kIndexLimit) {
		common::logError(kErrorTooManyTags, basePath_ + ".tags")
			<< "there are " << langpack_.tags.size()
			<< " tags, the limit is " << kIndexLimit
			<< ". Delete this file to drop the tags that are not used anymore.";
		return false;
	}
	header_ = std::make_unique<common::CppFile>(basePath_ + ".h", project_);
	header_->include("lang/lang_tag.h").include("lang/lang_values.h").newline();

	writeHeaderTagTypes();
	writeHeaderInterface();
	writeHeaderReactiveInterface();
	writeHeaderKeysInclude();

	return header_->finalize();
}

void Generator::writeHeaderKeysInclude() {
	header_->stream() << "\
#ifdef LANG_KEYS_SUBSET\n\
#include LANG_KEYS_SUBSET\n\
#else // LANG_KEYS_SUBSET\n\
#include \"" << baseName_ << "_keys.h\"\n\
#endif // LANG_KEYS_SUBSET\n";
}

bool Generator::writeCounts() {
	auto file = common::CppFile(basePath_ + "_counts.h", project_);
	file.pushNamespace("Lang").stream() << "\
\n\
inline constexpr auto kTagsCount = ushort(" << langpack_.tags.size() << ");\n\
inline constexpr auto kKeysCount = ushort(" << keysCount_ << ");\n\
\n";
	file.popNamespace();
	return file.finalize();
}

bool Generator::writeAllKeys() {
	auto file = common::CppFile(basePath_ + "_keys.h", project_);
	file.include(baseName_ + ".h").newline();
	file.pushNamespace("tr").newline();
	for (const auto &[combination, code] : specializations_) {
		file.stream() << code;
	}
	for (const auto &declaration : declarations_) {
		file.stream() << declaration << "\n";
	}
	file.newline().popNamespace();
	return file.finalize();
}

void Generator::writeHeaderTagTypes() {
	auto index = 0;
	for (auto &tag : langpack_.tags) {
		if (tag.tag == kPluralTags[0]) {
			auto elements = QStringList();
			header_->stream()
				<< "enum lngtag_" << tag.tag << " : int { ";
			for (auto i = 0; i != kPluralTags.size(); ++i) {
				elements.push_back("lt_" + kPluralTags[i] + " = " + QString::number(index + i * 1000));
			}
			header_->stream() << elements.join(", ") << " };\n";
			++index;
		} else {
			header_->stream() << "enum lngtag_" << tag.tag << " : int { lt_" << tag.tag << " = " << index++ << " };\n";
		}
	}
	header_->newline();
}

void Generator::writeHeaderInterface() {
	header_->pushNamespace("Lang").stream() << "\
\n\
ushort GetTagIndex(QLatin1String tag);\n\
ushort GetKeyIndex(QLatin1String key);\n\
bool IsTagReplaced(ushort key, ushort tag);\n\
QString GetOriginalValue(ushort key);\n\
\n";
	writeHeaderTagValueLookup();
	header_->popNamespace().newline();
}

void Generator::writeHeaderTagValueLookup() {
	header_->pushNamespace("details").stream() << "\
\n\
template <typename Tag>\n\
struct TagData;\n\
\n\
template <typename Tag>\n\
inline constexpr ushort TagValue() {\n\
	return TagData<Tag>::value;\n\
}\n\
\n";

	for (auto &tag : langpack_.tags) {
		header_->stream() << "template <> struct TagData<lngtag_" << tag.tag << "> : std::integral_constant<ushort, ushort(lt_" << tag.tag << ")> {};\n";
	}

	header_->newline().popNamespace();
}

void Generator::writeHeaderReactiveInterface() {
	header_->pushNamespace("tr");

	writeHeaderProducersInterface();

	header_->popNamespace().newline();
}

void Generator::writeHeaderProducersInterface() {
	header_->pushNamespace("details").stream() << "\
\n\
struct Identity {\n\
	QString operator()(const QString &value) const {\n\
		return value;\n\
	}\n\
};\n\
\n";

	header_->popNamespace().newline();
	header_->stream() << "\
struct now_t {\n\
};\n\
\n\
inline constexpr now_t now{};\n\
\n\
inline auto to_count() {\n\
	return rpl::map([](auto value) {\n\
		return float64(value);\n\
	});\n\
}\n\
\n\
template <typename P>\n\
using S = std::decay_t<decltype(std::declval<P>()(QString()))>;\n\
\n\
template <typename ...Tags>\n\
struct phrase;\n\
\n";
	auto specializations = std::map<QString, QString>();
	for (auto &entry : langpack_.entries) {
		const auto isPlural = !entry.keyBase.isEmpty();
		auto tags = QStringList();
		auto producerArgs = QStringList();
		auto currentArgs = QStringList();
		auto values = QStringList();
		values.push_back("base");
		values.push_back("std::move(p)");
		for (auto &tagData : entry.tags) {
			const auto &tag = tagData.tag;
			const auto isPluralTag = isPlural && (tag == kPluralTags[0]);
			tags.push_back("lngtag_" + tag);
			const auto type1 = "lngtag_" + tag;
			const auto arg1 = type1 + (isPluralTag ? " type" : "");
			const auto producerType2 = (isPluralTag ? "rpl::producer<float64> " : "rpl::producer<S<P>> ");
			const auto producerArg2 = producerType2 + tag + "__val";
			const auto currentType2 = (isPluralTag ? "float64 " : "const S<P> &");
			const auto currentArg2 = currentType2 + tag + "__val";
			producerArgs.push_back(arg1 + ", " + producerArg2);
			currentArgs.push_back(arg1 + ", " + currentArg2);
			if (isPluralTag) {
				values.push_back("type");
			}
			values.push_back(tag + "__val");
		}
		producerArgs.push_back("P p = P()");
		currentArgs.push_back("P p = P()");
		const auto combination = tags.join(',');
		if (specializations.find(combination) != specializations.end()) {
			continue;
		}
		specializations.emplace(combination, "\
template <>\n\
struct phrase<" + tags.join(", ") + "> {\n\
	template <typename P = details::Identity>\n\
	rpl::producer<S<P>> operator()(" + producerArgs.join(", ") + ") const {\n\
		return ::Lang::details::Producer<" + tags.join(", ") + ">::Combine(" + values.join(", ") + ");\n\
	}\n\
\n\
	template <typename P = details::Identity>\n\
	S<P> operator()(now_t, " + currentArgs.join(", ") + ") const {\n\
		return ::Lang::details::Producer<" + tags.join(", ") + ">::Current(" + values.join(", ") + ");\n\
	}\n\
\n\
	ushort base;\n\
};\n\
\n");
	}
	const auto plain = specializations.find(QString());
	if (plain != specializations.end()) {
		header_->stream() << plain->second;
		specializations.erase(plain);
	}
	specializations_ = std::move(specializations);
}

bool Generator::writeSource() {
	source_ = std::make_unique<common::CppFile>(basePath_ + ".cpp", project_);

	source_->include("lang/lang_keys.h")
		.include(baseName_ + "_counts.h")
		.pushNamespace("Lang")
		.pushNamespace();

	source_->stream() << "\
static_assert(sizeof(QChar) == sizeof(char16_t));\n\
static_assert(alignof(QChar) == alignof(char16_t));\n\
\n";

	auto byIndex = std::vector<const LangPack::Entry*>(keysCount_, nullptr);
	for (auto i = 0, count = int(langpack_.entries.size()); i != count; ++i) {
		byIndex[indices_[i]] = &langpack_.entries[i];
	}

	source_->stream() << "\
const char16_t DefaultData[] = {";
	auto count = 0;
	auto fulllength = 0;
	for (const auto entry : byIndex) {
		if (!entry) {
			continue;
		}
		for (auto ch : entry->value) {
			if (fulllength > 0) source_->stream() << ",";
			if (!count++) {
				source_->stream() << "\n";
			} else {
				if (count == 12) {
					count = 0;
				}
				source_->stream() << " ";
			}
			source_->stream() << "0x" << QString::number(ch.unicode(), 16);
			++fulllength;
		}
	}
	source_->stream() << " };\n\
\n\
int Offsets[] = {";
	count = 0;
	auto offset = 0;
	auto written = 0;
	auto writeOffset = [this, &count, &offset, &written] {
		if (written++ > 0) source_->stream() << ",";
		if (!count++) {
			source_->stream() << "\n";
		} else {
			if (count == 12) {
				count = 0;
			}
			source_->stream() << " ";
		}
		source_->stream() << offset;
	};
	for (const auto entry : byIndex) {
		writeOffset();
		offset += entry ? entry->value.size() : 0;
	}
	writeOffset();
	source_->stream() << " };\n";
	source_->popNamespace().stream() << "\
\n\
ushort GetTagIndex(QLatin1String tag) {\n\
	auto size = tag.size();\n\
	auto data = tag.data();\n";

	auto tagsSet = std::set<QString, std::greater<>>();
	for (auto &tag : langpack_.tags) {
		tagsSet.insert(tag.tag);
	}

	writeSetSearch(tagsSet, [](const QString &tag) {
		return "ushort(lt_" + tag + ")";
	}, "kTagsCount");

	source_->stream() << "\
}\n\
\n";

	auto indices = std::map<QString, QString>();
	for (auto i = 0, total = int(langpack_.entries.size()); i != total; ++i) {
		indices.emplace(
			getFullKey(langpack_.entries[i]),
			QString::number(indices_[i]));
	}
	const auto indexOfKey = [&](const QString &full) {
		const auto i = indices.find(full);
		if (i == indices.end()) {
			return QString();
		}
		return i->second;
	};

	auto taggedKeys = std::map<QString, QString>();
	auto keysSet = std::set<QString, std::greater<>>();
	for (auto &entry : langpack_.entries) {
		if (!entry.keyBase.isEmpty()) {
			for (auto i = 0; i != kPluralPartCount; ++i) {
				auto keyName = entry.keyBase + '#' + kPluralParts[i];
				taggedKeys.emplace(keyName, ComputePluralKey(entry.keyBase, i));
				keysSet.insert(keyName);
			}
		} else {
			auto full = getFullKey(entry);
			if (full != entry.key) {
				taggedKeys.emplace(entry.key, full);
			}
			keysSet.insert(entry.key);
		}
	}

	// One huge function does not compile in MSVC for ARM64 (C1053).
	constexpr auto kKeysPerPart = 1500;
	const auto computeKey = [&](const QString &key) {
		auto it = taggedKeys.find(key);
		const auto name = (it != taggedKeys.end()) ? it->second : key;
		return indexOfKey(name);
	};
	auto partsCount = 0;
	for (auto i = keysSet.begin(); i != keysSet.end();) {
		auto part = std::set<QString, std::greater<>>();
		for (; i != keysSet.end() && int(part.size()) != kKeysPerPart; ++i) {
			part.insert(*i);
		}
		source_->stream() << "\
static ushort GetKeyIndexPart" << partsCount << "(int size, const char *data) {\n";
		writeSetSearch(part, computeKey, "kKeysCount");
		source_->stream() << "\
}\n\
\n";
		++partsCount;
	}
	source_->stream() << "\
ushort GetKeyIndex(QLatin1String key) {\n\
	const auto size = int(key.size());\n\
	const auto data = key.data();\n";
	for (auto i = 0; i != partsCount; ++i) {
		source_->stream() << "\
	if (const auto index = GetKeyIndexPart" << i << "(size, data); index != kKeysCount) {\n\
		return index;\n\
	}\n";
	}
	source_->stream() << "\
	return kKeysCount;\n";
	header_->popNamespace().newline();

	source_->stream() << "\
}\n\
\n\
bool IsTagReplaced(ushort key, ushort tag) {\n\
	switch (key) {\n";

	auto lastWrittenPluralEntry = QString();
	for (auto &entry : langpack_.entries) {
		if (entry.tags.empty()) {
			continue;
		}
		if (!entry.keyBase.isEmpty()) {
			if (entry.keyBase == lastWrittenPluralEntry) {
				continue;
			}
			lastWrittenPluralEntry = entry.keyBase;
			for (auto i = 0; i != kPluralPartCount; ++i) {
				source_->stream() << "\
	case " << indexOfKey(ComputePluralKey(entry.keyBase, i)) << ":" << ((i + 1 == kPluralPartCount) ? " {" : "") << "\n";
			}
		} else {
			source_->stream() << "\
	case " << indexOfKey(getFullKey(entry)) << ": {\n";
		}
		source_->stream() << "\
		switch (tag) {\n";
		for (auto &tag : entry.tags) {
			source_->stream() << "\
		case lt_" << tag.tag << ":\n";
		}
		source_->stream() << "\
			return true;\n\
		}\n\
	} break;\n";
	}

	source_->stream() << "\
	}\
\n\
	return false;\n\
}\n\
\n\
QString GetOriginalValue(ushort key) {\n\
	Expects(key < kKeysCount);\n\
\n\
	const auto offset = Offsets[key];\n\
	return QString::fromRawData(\n\
		reinterpret_cast<const QChar*>(DefaultData + offset),\n\
		Offsets[key + 1] - offset);\n\
}\n\
\n";

	return source_->finalize();
}

template <typename ComputeResult>
void Generator::writeSetSearch(const std::set<QString, std::greater<>> &set, ComputeResult computeResult, const QString &invalidResult) {
	auto tabs = [](int size) {
		return QString(size, '\t');
	};

	enum class UsedCheckType {
		Switch,
		If,
		UpcomingIf,
	};
	auto checkTypes = QVector<UsedCheckType>();
	auto checkLengthHistory = QVector<int>();
	auto chars = QString();
	auto tabsUsed = 1;

	// Returns true if at least one check was finished.
	auto finishChecksTillKey = [this, &chars, &checkTypes, &checkLengthHistory, &tabsUsed, tabs](const QString &key) {
		auto result = false;
		while (!chars.isEmpty() && !key.startsWith(chars)) {
			result = true;

			auto wasType = checkTypes.back();
			chars.resize(chars.size() - 1);
			checkTypes.pop_back();
			if (wasType == UsedCheckType::Switch || wasType == UsedCheckType::If) {
				--tabsUsed;
				if (wasType == UsedCheckType::Switch) {
					source_->stream() << tabs(tabsUsed) << "break;\n";
				}
				if ((!chars.isEmpty() && !key.startsWith(chars)) || key == chars) {
					source_->stream() << tabs(tabsUsed) << "}\n";
					checkLengthHistory.pop_back();
				}
			}
		}
		return result;
	};

	// Check if we can use "if" for a check on "charIndex" in "it" (otherwise only "switch")
	auto canUseIfForCheck = [](auto it, auto end, int charIndex) {
		auto key = *it;
		auto i = it;
		auto keyStart = key.mid(0, charIndex);
		for (++i; i != end; ++i) {
			auto nextKey = *i;
			if (nextKey.mid(0, charIndex) != keyStart) {
				return true;
			} else if (nextKey.size() > charIndex && nextKey[charIndex] != key[charIndex]) {
				return false;
			}
		}
		return true;
	};

	auto countMinimalLength = [](auto it, auto end, int charIndex) {
		auto key = *it;
		auto i = it;
		auto keyStart = key.mid(0, charIndex);
		auto result = key.size();
		for (++i; i != end; ++i) {
			auto nextKey = *i;
			if (nextKey.mid(0, charIndex) != keyStart) {
				break;
			} else if (nextKey.size() > charIndex && result > nextKey.size()) {
				result = nextKey.size();
			}
		}
		return result;
	};

	for (auto i = set.begin(), e = set.end(); i != e; ++i) {
		// If we use just "auto" here and "name" becomes mutable,
		// the operator[] will return QCharRef instead of QChar,
		// and "auto ch = name[index]" will behave like "auto &ch =",
		// if you assign something to "ch" after that you'll change "name" (!)
		const auto name = *i;

		auto weContinueOldSwitch = finishChecksTillKey(name);
		while (chars.size() != name.size()) {
			auto checking = chars.size();

			auto keyChar = name[checking];
			auto usedIfForCheckCount = 0;
			auto minimalLengthCheck = int(countMinimalLength(i, e, checking));
			for (; checking + usedIfForCheckCount != name.size(); ++usedIfForCheckCount) {
				if (!canUseIfForCheck(i, e, checking + usedIfForCheckCount)
					|| countMinimalLength(i, e, checking + usedIfForCheckCount) != minimalLengthCheck) {
					break;
				}
			}
			auto usedIfForCheck = !weContinueOldSwitch && (usedIfForCheckCount > 0);
			const auto checkedLength = checkLengthHistory.isEmpty()
				? 0
				: checkLengthHistory.back();
			const auto requiredLength = std::max(
				minimalLengthCheck,
				checkedLength);
			auto checkLengthCondition = QString();
			if (weContinueOldSwitch) {
				weContinueOldSwitch = false;
			} else {
				checkLengthCondition = (requiredLength > checkedLength) ? ("size >= " + QString::number(requiredLength)) : QString();
				if (!usedIfForCheck) {
					source_->stream() << tabs(tabsUsed) << (checkLengthCondition.isEmpty() ? QString() : ("if (" + checkLengthCondition + ") ")) << "switch (data[" << checking << "]) {\n";
					checkLengthHistory.push_back(requiredLength);
				}
			}
			if (usedIfForCheck) {
				auto conditions = QStringList();
				if (usedIfForCheckCount > 1) {
					conditions.push_back("!memcmp(data + " + QString::number(checking) + ", \"" + name.mid(checking, usedIfForCheckCount) + "\", " + QString::number(usedIfForCheckCount) + ")");
				} else {
					conditions.push_back("data[" + QString::number(checking) + "] == '" + keyChar + "'");
				}
				if (!checkLengthCondition.isEmpty()) {
					conditions.push_front(checkLengthCondition);
				}
				source_->stream() << tabs(tabsUsed) << "if (" << conditions.join(" && ") << ") {\n";
				checkLengthHistory.push_back(requiredLength);
				checkTypes.push_back(UsedCheckType::If);
				for (auto i = 1; i != usedIfForCheckCount; ++i) {
					checkTypes.push_back(UsedCheckType::UpcomingIf);
					chars.push_back(keyChar);
					keyChar = name[checking + i];
				}
			} else {
				source_->stream() << tabs(tabsUsed) << "case '" << keyChar << "':\n";
				checkTypes.push_back(UsedCheckType::Switch);
			}
			++tabsUsed;
			chars.push_back(keyChar);
		}
		source_->stream() << tabs(tabsUsed) << "return (size == " << chars.size() << ") ? " << computeResult(name) << " : " << invalidResult << ";\n";
	}
	finishChecksTillKey(QString());

	source_->stream() << "\
\n\
	return " << invalidResult << ";\n";
}

QString Generator::getFullKey(const LangPack::Entry &entry) {
	if (!entry.keyBase.isEmpty() || entry.tags.empty()) {
		return entry.key;
	}
	return entry.key + "__tagged";
}

} // namespace lang
} // namespace codegen
