// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "codegen/lang/subsets.h"

#include "codegen/common/logging.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <vector>
#include <QtCore/QByteArrayList>
#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>

namespace codegen {
namespace lang {
namespace {

constexpr int kErrorCantReadKeys    = 831;
constexpr int kErrorCantReadSource  = 832;
constexpr int kErrorCantWriteSubset = 833;

constexpr auto kCacheVersion = quint32(2);

const auto kKeysFile = QString("lang_auto_keys.h");
const auto kSubsetsFolder = QString("lang_subsets");
const auto kCacheFile = QString("lang_subsets/.cache");
const auto kDeclarationStart = QByteArray("inline constexpr phrase<");
const auto kSpecializationStart = QByteArray("struct phrase<");
const auto kPhraseUsage = QByteArray("phrase<");

[[nodiscard]] bool IsIdentifierChar(char ch) {
	return (ch >= 'a' && ch <= 'z')
		|| (ch >= 'A' && ch <= 'Z')
		|| (ch >= '0' && ch <= '9')
		|| (ch == '_');
}

[[nodiscard]] qsizetype KeyPrefixLength(const char *data, qsizetype left) {
	for (const auto prefix : { "lng_", "ayu_", "fresh_" }) {
		const auto length = qsizetype(qstrlen(prefix));
		if (left > length && !memcmp(data, prefix, length)) {
			return length;
		}
	}
	return 0;
}

struct Declarations {
	std::vector<QByteArray> code;
	QHash<QByteArray, int> byName;
	std::map<QByteArray, QByteArray> specializations;
	std::vector<QByteArray> combinations;
};

struct Scanned {
	qint64 modified = 0;
	qint64 size = 0;
	std::vector<QByteArray> tokens;
	std::vector<QByteArray> combinations;
	std::vector<QByteArray> includes;
};

struct SourceFile {
	QString absolute;
	QString relative;
	Scanned scanned;
	std::vector<int> keys;
	std::vector<int> included;
	bool unit = false;
	bool fresh = false;
};

struct Cache {
	QHash<QString, Scanned> scanned;
	QHash<QString, QByteArray> subsets;
	qint64 keysModified = 0;
	qint64 keysSize = 0;
};

constexpr auto kSaneCount = qint32(1024 * 1024);

QDataStream &operator<<(QDataStream &stream, const Scanned &value) {
	const auto list = [&](const std::vector<QByteArray> &values) {
		stream << qint32(values.size());
		for (const auto &entry : values) {
			stream << entry;
		}
	};
	stream << value.modified << value.size;
	list(value.tokens);
	list(value.combinations);
	list(value.includes);
	return stream;
}

QDataStream &operator>>(QDataStream &stream, Scanned &value) {
	const auto list = [&](std::vector<QByteArray> &values) {
		auto count = qint32(0);
		stream >> count;
		if (count < 0 || count > kSaneCount) {
			stream.setStatus(QDataStream::ReadCorruptData);
			return;
		}
		values.resize(count);
		for (auto &entry : values) {
			stream >> entry;
		}
	};
	stream >> value.modified >> value.size;
	list(value.tokens);
	list(value.combinations);
	list(value.includes);
	return stream;
}

[[nodiscard]] QByteArray NormalizeCombination(const QByteArray &tags) {
	auto result = QByteArrayList();
	for (const auto &tag : tags.split(',')) {
		const auto trimmed = tag.trimmed();
		if (!trimmed.startsWith("lngtag_")) {
			return QByteArray();
		}
		result.push_back(trimmed);
	}
	return result.join(',');
}

[[nodiscard]] bool ReadDeclarations(
		const QString &path,
		Declarations &result) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		common::logError(kErrorCantReadKeys, path)
			<< "can not open the generated keys header for reading";
		return false;
	}
	const auto content = file.readAll();
	file.close();

	auto block = QByteArray();
	auto blockCombination = QByteArray();
	auto inside = false;
	for (auto from = qsizetype(0); from < content.size();) {
		auto till = content.indexOf('\n', from);
		if (till < 0) {
			till = content.size();
		}
		const auto line = content.mid(from, till - from);
		from = till + 1;

		if (inside) {
			block += line + '\n';
			if (line == "};") {
				result.specializations.emplace(blockCombination, block);
				inside = false;
			}
			continue;
		} else if (line.startsWith(kSpecializationStart)) {
			const auto close = line.lastIndexOf('>');
			blockCombination = NormalizeCombination(line.mid(
				kSpecializationStart.size(),
				close - kSpecializationStart.size()));
			block = "template <>\n" + line + '\n';
			inside = true;
			continue;
		} else if (!line.startsWith(kDeclarationStart)) {
			continue;
		}
		const auto after = line.indexOf("> ", kDeclarationStart.size() - 1);
		const auto brace = (after < 0) ? -1 : line.indexOf('{', after + 2);
		if (after < 0 || brace < 0) {
			common::logError(kErrorCantReadKeys, path)
				<< "bad key declaration: " << line.constData();
			return false;
		}
		result.byName.insert(
			line.mid(after + 2, brace - after - 2).trimmed(),
			int(result.code.size()));
		result.combinations.push_back(NormalizeCombination(line.mid(
			kDeclarationStart.size(),
			after - kDeclarationStart.size())));
		result.code.push_back(line);
	}
	if (result.code.empty()) {
		common::logError(kErrorCantReadKeys, path)
			<< "no key declarations found";
		return false;
	}
	return true;
}

void Scan(const QByteArray &content, Scanned &result) {
	const auto data = content.constData();
	const auto size = content.size();
	for (auto i = qsizetype(0); i + 4 < size; ++i) {
		const auto prefix = KeyPrefixLength(data + i, size - i);
		if (!prefix
			|| (i > 0 && IsIdentifierChar(data[i - 1]))) {
			continue;
		}
		auto till = i + prefix;
		while (till != size && IsIdentifierChar(data[till])) {
			++till;
		}
		result.tokens.push_back(content.mid(i, till - i));
		i = till - 1;
	}
	for (auto from = qsizetype(0); from != size;) {
		const auto found = content.indexOf(kPhraseUsage, from);
		if (found < 0) {
			break;
		}
		from = found + kPhraseUsage.size();
		const auto close = content.indexOf('>', from);
		if (close < 0) {
			break;
		}
		const auto combination = NormalizeCombination(
			content.mid(from, close - from));
		if (!combination.isEmpty()) {
			result.combinations.push_back(combination);
		}
		from = close + 1;
	}
	const auto directive = QByteArray("#include");
	for (auto from = qsizetype(0); from != size;) {
		const auto found = content.indexOf(directive, from);
		if (found < 0) {
			break;
		}
		from = found + directive.size();

		auto lineStart = found;
		while (lineStart > 0 && data[lineStart - 1] != '\n') {
			--lineStart;
		}
		auto clean = true;
		for (auto i = lineStart; i != found; ++i) {
			if (data[i] != ' ' && data[i] != '\t') {
				clean = false;
				break;
			}
		}
		if (!clean) {
			continue;
		}
		auto quote = from;
		while (quote != size && (data[quote] == ' ' || data[quote] == '\t')) {
			++quote;
		}
		if (quote == size || data[quote] != '"') {
			continue;
		}
		const auto till = content.indexOf('"', quote + 1);
		if (till < 0) {
			break;
		}
		result.includes.push_back(content.mid(quote + 1, till - quote - 1));
		from = till + 1;
	}
}

[[nodiscard]] Cache ReadCache(const QString &path) {
	auto result = Cache();
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	auto stream = QDataStream(&file);
	auto version = quint32(0);
	stream >> version;
	if (version != kCacheVersion) {
		return Cache();
	}
	stream >> result.keysModified >> result.keysSize;
	stream >> result.scanned >> result.subsets;
	return (stream.status() == QDataStream::Ok) ? result : Cache();
}

void WriteCache(const QString &path, const Cache &cache) {
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	auto stream = QDataStream(&file);
	stream << kCacheVersion << cache.keysModified << cache.keysSize;
	stream << cache.scanned << cache.subsets;
}

[[nodiscard]] QByteArray Signature(
		const std::vector<int> &keys,
		const std::set<QByteArray> &combinations) {
	auto result = QByteArray();
	for (const auto key : keys) {
		result += QByteArray::number(key) + ' ';
	}
	result += '|';
	for (const auto &combination : combinations) {
		result += combination + ' ';
	}
	return result;
}

[[nodiscard]] bool WriteSubset(
		const QString &path,
		const Declarations &declarations,
		const std::vector<int> &keys,
		const std::set<QByteArray> &combinations,
		const common::ProjectInfo &project) {
	auto file = common::CppFile(path, project);
	file.include("lang_auto.h").newline();
	file.pushNamespace("tr").newline();
	for (const auto &combination : combinations) {
		const auto i = declarations.specializations.find(combination);
		if (i != declarations.specializations.end()) {
			file.stream() << i->second;
		}
	}
	for (const auto index : keys) {
		file.stream() << declarations.code[index] << "\n";
	}
	file.newline().popNamespace();
	if (!file.finalize()) {
		common::logError(kErrorCantWriteSubset, path)
			<< "can not write the lang keys subset";
		return false;
	}
	return true;
}

} // namespace

bool WriteSubsets(
		const QString &genPath,
		const QString &sourcesPath,
		const common::ProjectInfo &project) {
	const auto keysPath = genPath + '/' + kKeysFile;
	const auto keysInfo = QFileInfo(keysPath);
	auto cache = ReadCache(genPath + '/' + kCacheFile);
	const auto keysSame = (cache.keysModified
		== keysInfo.lastModified().toMSecsSinceEpoch())
		&& (cache.keysSize == keysInfo.size());

	const auto root = QDir(sourcesPath).absolutePath();
	auto files = std::vector<SourceFile>();
	auto indices = QHash<QString, int>();
	auto iterator = QDirIterator(
		root,
		QDir::Files,
		QDirIterator::Subdirectories);
	while (iterator.hasNext()) {
		const auto absolute = iterator.next();
		if (!absolute.endsWith(".cpp")
			&& !absolute.endsWith(".h")
			&& !absolute.endsWith(".mm")) {
			continue;
		}
		const auto info = iterator.fileInfo();
		auto file = SourceFile();
		file.absolute = absolute;
		file.relative = absolute.mid(root.size() + 1);
		file.unit = absolute.endsWith(".cpp") || absolute.endsWith(".mm");
		file.scanned.modified = info.lastModified().toMSecsSinceEpoch();
		file.scanned.size = info.size();
		files.push_back(std::move(file));
	}
	std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) {
		return a.relative < b.relative;
	});
	for (auto i = 0, count = int(files.size()); i != count; ++i) {
		indices.insert(files[i].absolute, i);
	}

	auto dirty = !keysSame || (int(cache.scanned.size()) != int(files.size()));
	for (auto &file : files) {
		const auto i = cache.scanned.constFind(file.relative);
		if (i != cache.scanned.cend()
			&& i->modified == file.scanned.modified
			&& i->size == file.scanned.size) {
			file.scanned = *i;
			continue;
		}
		auto device = QFile(file.absolute);
		if (!device.open(QIODevice::ReadOnly)) {
			common::logError(kErrorCantReadSource, file.absolute)
				<< "can not open the source file for reading";
			return false;
		}
		auto scanned = Scanned();
		scanned.modified = file.scanned.modified;
		scanned.size = file.scanned.size;
		Scan(device.readAll(), scanned);
		if (i == cache.scanned.cend()
			|| i->tokens != scanned.tokens
			|| i->combinations != scanned.combinations
			|| i->includes != scanned.includes) {
			dirty = true;
		}
		file.scanned = std::move(scanned);
	}

	const auto subsets = genPath + '/' + kSubsetsFolder;
	if (!dirty) {
		auto complete = true;
		for (const auto &file : files) {
			if (file.unit
				&& !QFileInfo::exists(subsets + '/' + file.relative + ".h")) {
				complete = false;
				break;
			}
		}
		if (complete) {
			auto next = Cache();
			next.keysModified = keysInfo.lastModified().toMSecsSinceEpoch();
			next.keysSize = keysInfo.size();
			next.subsets = cache.subsets;
			for (const auto &file : files) {
				next.scanned.insert(file.relative, file.scanned);
			}
			WriteCache(genPath + '/' + kCacheFile, next);
			return true;
		}
	}

	auto declarations = Declarations();
	if (!ReadDeclarations(keysPath, declarations)) {
		return false;
	}

	for (auto &file : files) {
		for (const auto &token : file.scanned.tokens) {
			const auto i = declarations.byName.constFind(token);
			if (i != declarations.byName.cend()) {
				file.keys.push_back(*i);
			}
		}
		const auto dir = file.absolute.left(file.absolute.lastIndexOf('/'));
		for (const auto &include : file.scanned.includes) {
			const auto relative = QString::fromUtf8(include);
			const auto exact = !relative.contains("..")
				&& !relative.contains("./");
			for (const auto &base : { dir, root }) {
				const auto joined = base + '/' + relative;
				const auto i = indices.constFind(
					exact ? joined : QDir::cleanPath(joined));
				if (i != indices.cend()) {
					file.included.push_back(*i);
					break;
				}
			}
		}
	}

	auto next = Cache();
	next.keysModified = keysInfo.lastModified().toMSecsSinceEpoch();
	next.keysSize = keysInfo.size();

	auto visited = std::vector<int>(files.size(), -1);
	auto taken = std::vector<int>(declarations.code.size(), -1);
	auto queue = std::vector<int>();
	auto keys = std::vector<int>();
	auto combinations = std::set<QByteArray>();
	for (auto i = 0, count = int(files.size()); i != count; ++i) {
		next.scanned.insert(files[i].relative, files[i].scanned);
		if (!files[i].unit) {
			continue;
		}
		keys.clear();
		combinations.clear();
		queue.clear();
		queue.push_back(i);
		visited[i] = i;
		for (auto j = 0; j != int(queue.size()); ++j) {
			const auto &file = files[queue[j]];
			for (const auto key : file.keys) {
				if (taken[key] != i) {
					taken[key] = i;
					keys.push_back(key);
					const auto &combination = declarations.combinations[key];
					if (!combination.isEmpty()) {
						combinations.emplace(combination);
					}
				}
			}
			for (const auto &combination : file.scanned.combinations) {
				combinations.emplace(combination);
			}
			for (const auto included : file.included) {
				if (visited[included] != i) {
					visited[included] = i;
					queue.push_back(included);
				}
			}
		}
		std::sort(keys.begin(), keys.end());
		const auto signature = Signature(keys, combinations);
		next.subsets.insert(files[i].relative, signature);

		const auto path = subsets + '/' + files[i].relative + ".h";
		const auto known = cache.subsets.constFind(files[i].relative);
		if (keysSame
			&& known != cache.subsets.cend()
			&& *known == signature
			&& QFileInfo::exists(path)) {
			continue;
		} else if (!WriteSubset(
				path,
				declarations,
				keys,
				combinations,
				project)) {
			return false;
		}
	}
	WriteCache(genPath + '/' + kCacheFile, next);
	return true;
}

} // namespace lang
} // namespace codegen
