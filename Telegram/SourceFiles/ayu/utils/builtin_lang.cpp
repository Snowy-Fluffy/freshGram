#include "ayu/utils/builtin_lang.h"

#include "base/debug_log.h"
#include "lang/lang_file_parser.h"

#include <QtCore/QFile>

#include <map>

namespace FreshLang {
namespace {

[[nodiscard]] QString ResourceCode(const QString &languageId) {
	auto id = languageId.toLower();
	if (id.startsWith(u"classic-"_q)) {
		id = id.mid(8);
	}
	if (id.startsWith(u"zh"_q)) {
		return u"zh"_q;
	}
	const auto code = id.section('-', 0, 0);
	return (code == u"ru"_q
		|| code == u"it"_q
		|| code == u"de"_q
		|| code == u"fr"_q)
		? code
		: QString();
}

[[nodiscard]] Strings Load(const QString &code) {
	auto file = QFile(u":/gui/fresh_lang/%1.strings"_q.arg(code));
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	auto result = Strings();
	const auto content = file.readAll();
	Lang::FileParser loader(content, [&](
			QLatin1String key,
			const QByteArray &value) {
		result.emplace_back(QByteArray(key.data(), key.size()), value);
	});
	if (!loader.errors().isEmpty()) {
		LOG(("Lang Error: freshGram %1 strings: %2").arg(
			code,
			loader.errors()));
	}
	return result;
}

} // namespace

const Strings &BuiltinStrings(const QString &languageId) {
	static auto cache = std::map<QString, Strings>();
	static const auto empty = Strings();
	const auto code = ResourceCode(languageId);
	if (code.isEmpty()) {
		return empty;
	}
	const auto i = cache.find(code);
	return (i != cache.end())
		? i->second
		: cache.emplace(code, Load(code)).first->second;
}

} // namespace FreshLang
