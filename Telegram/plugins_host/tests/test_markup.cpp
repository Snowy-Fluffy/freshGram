#include "check.h"
#include "fgpl/text_markup.h"

#include <string>

namespace {

using namespace fgpl;

[[nodiscard]] MarkedText Parse(TextFormat format, const std::string &source) {
	auto result = ParseMarkup(format, source);
	CHECK(result.ok());
	return result.ok() ? result.value() : MarkedText();
}

[[nodiscard]] bool Fails(TextFormat format, const std::string &source) {
	return !ParseMarkup(format, source).ok();
}

[[nodiscard]] bool Is(
		const TextEntity &entity,
		const char *type,
		int64_t offset,
		int64_t length) {
	return entity.type == type && entity.offset == offset && entity.length == length;
}

void TestMarkdown() {
	const auto md = [](const std::string &source) {
		return Parse(TextFormat::Markdown, source);
	};
	auto result = md("**bold** and __it__");
	CHECK(result.text == "bold and it");
	CHECK(result.entities.size() == 2);
	CHECK(Is(result.entities[0], "bold", 0, 4) && Is(result.entities[1], "italic", 9, 2));

	result = md("~~gone~~ ||secret|| `x`");
	CHECK(result.text == "gone secret x" && result.entities.size() == 3);
	CHECK(Is(result.entities[0], "strike", 0, 4));
	CHECK(Is(result.entities[1], "spoiler", 5, 6));
	CHECK(Is(result.entities[2], "code", 12, 1));

	result = md("\\*\\*not\\*\\* bold");
	CHECK(result.text == "**not** bold" && result.entities.empty());

	result = md("**a __b__ c**");
	CHECK(result.text == "a b c" && result.entities.size() == 2);
	CHECK(Is(result.entities[0], "bold", 0, 5) && Is(result.entities[1], "italic", 2, 1));

	result = md("a ** b");
	CHECK(result.text == "a ** b" && result.entities.empty());
	CHECK(Fails(TextFormat::Markdown, "**a __b** c__"));

	result = md("```py\nprint(1)\n``` after");
	CHECK(result.text == "print(1) after" && result.entities.size() == 1);
	CHECK(Is(result.entities[0], "pre", 0, 8) && result.entities[0].language == "py");
	result = md("```\nplain\n```");
	CHECK(result.text == "plain" && result.entities[0].language.empty());

	result = md("[site](https://example.com/a?b=1) and [me](tg://user?id=5)");
	CHECK(result.text == "site and me" && result.entities.size() == 2);
	CHECK(Is(result.entities[0], "text_url", 0, 4) && result.entities[0].url == "https://example.com/a?b=1");
	CHECK(Is(result.entities[1], "mention_name", 9, 2) && result.entities[1].id == 5);
	result = md("[x](javascript:alert) [y](nothing)");
	CHECK(result.text == "[x](javascript:alert) [y](nothing)" && result.entities.empty());
	result = md("[**b**](https://example.com)");
	CHECK(result.text == "b" && result.entities.size() == 2);

	result = md("hi ![\xF0\x9F\x99\x82](tg://emoji?id=123) there");
	CHECK(result.text == "hi \xF0\x9F\x99\x82 there" && result.entities.size() == 1);
	CHECK(Is(result.entities[0], "custom_emoji", 3, 2) && result.entities[0].id == 123);
	result = md("![x](tg://emoji?id=0) ![](tg://emoji?id=4)");
	for (const auto &entity : result.entities) {
		CHECK(entity.type != "custom_emoji");
	}

	result = md("\xF0\x9F\x98\x80 **a**");
	CHECK(result.text == "\xF0\x9F\x98\x80 a" && Is(result.entities[0], "bold", 3, 1));

	result = md("> hello\n> world\nafter");
	CHECK(result.text == "hello\nworld\nafter" && result.entities.size() == 1);
	CHECK(Is(result.entities[0], "blockquote", 0, 11) && !result.entities[0].collapsed);
	result = md("before\n**> hidden\n> more\nafter");
	CHECK(result.text == "before\nhidden\nmore\nafter" && result.entities.size() == 1);
	CHECK(Is(result.entities[0], "blockquote", 7, 11) && result.entities[0].collapsed);
	result = md("```\n> not a quote\n```");
	CHECK(result.text == "> not a quote" && result.entities.size() == 1 && result.entities[0].type == "pre");

	auto many = std::string();
	for (auto i = 0; i != 300; ++i) {
		many += "**a** ";
	}
	CHECK(Fails(TextFormat::Markdown, many));
	CHECK(md("").text.empty());
}

void TestHtml() {
	const auto html = [](const std::string &source) {
		return Parse(TextFormat::Html, source);
	};
	auto result = html("<b>x</b> &amp; <i>y</i> <u>u</u> <s>s</s>");
	CHECK(result.text == "x & y u s" && result.entities.size() == 4);
	CHECK(Is(result.entities[0], "bold", 0, 1) && Is(result.entities[1], "italic", 4, 1));
	CHECK(Is(result.entities[2], "underline", 6, 1) && Is(result.entities[3], "strike", 8, 1));

	result = html("<STRONG>a</STRONG><em>b</em><ins>c</ins><del>d</del><strike>e</strike>");
	CHECK(result.entities.size() == 5);

	result = html("<pre><code class=\"language-cpp\">int x;</code></pre>");
	CHECK(result.text == "int x;" && result.entities.size() == 1);
	CHECK(Is(result.entities[0], "pre", 0, 6) && result.entities[0].language == "cpp");
	result = html("<pre>plain</pre> <code>c</code>");
	CHECK(result.entities.size() == 2 && result.entities[0].language.empty());

	result = html("<a href=\"https://example.com\">link</a> <a href='tg://user?id=9'>me</a>");
	CHECK(result.entities.size() == 2);
	CHECK(Is(result.entities[0], "text_url", 0, 4) && result.entities[0].url == "https://example.com");
	CHECK(Is(result.entities[1], "mention_name", 5, 2) && result.entities[1].id == 9);

	result = html("<blockquote>a</blockquote><blockquote expandable>b</blockquote>");
	CHECK(result.entities.size() == 2 && !result.entities[0].collapsed && result.entities[1].collapsed);

	result = html("<tg-spoiler>s</tg-spoiler><span class=\"tg-spoiler\">t</span>");
	CHECK(result.entities.size() == 2 && result.entities[0].type == "spoiler");

	result = html("<emoji id=\"77\">\xF0\x9F\x99\x82</emoji><tg-emoji emoji-id=\"78\">x</tg-emoji>");
	CHECK(result.entities.size() == 2);
	CHECK(Is(result.entities[0], "custom_emoji", 0, 2) && result.entities[0].id == 77);
	CHECK(Is(result.entities[1], "custom_emoji", 2, 1) && result.entities[1].id == 78);

	result = html("a<br>b<br/>c &lt;&gt;&quot;&#65;&#x1F600; AT&T");
	CHECK(result.text == "a\nb\nc <>\"A\xF0\x9F\x98\x80 AT&T" && result.entities.empty());

	CHECK(Fails(TextFormat::Html, "<font>x</font>"));
	CHECK(Fails(TextFormat::Html, "<b>x</i>"));
	CHECK(Fails(TextFormat::Html, "<b>x"));
	CHECK(Fails(TextFormat::Html, "x</b>"));
	CHECK(Fails(TextFormat::Html, "<a href=\"javascript:x\">x</a>"));
	CHECK(Fails(TextFormat::Html, "<a>x</a>"));
	CHECK(Fails(TextFormat::Html, "<emoji id=\"x\">a</emoji>"));
	CHECK(Fails(TextFormat::Html, "<emoji id=\"5\"></emoji>"));
	CHECK(Fails(TextFormat::Html, "<span class=\"other\">x</span>"));
	CHECK(Fails(TextFormat::Html, "1 < 2"));
	CHECK(Fails(TextFormat::Html, "<b title=\"x>y</b>"));
	CHECK(Fails(TextFormat::Html, "<pre><code class=\"language-a b\">x</code></pre>"));
	auto deep = std::string();
	for (auto i = 0; i != 40; ++i) {
		deep += "<b>";
	}
	CHECK(Fails(TextFormat::Html, deep));
}

void TestBasics() {
	const auto plain = Parse(TextFormat::Plain, "**not** <b>markup</b>");
	CHECK(plain.text == "**not** <b>markup</b>" && plain.entities.empty());
	CHECK(TextFormatNamed("plain") == TextFormat::Plain);
	CHECK(TextFormatNamed("markdown") == TextFormat::Markdown);
	CHECK(TextFormatNamed("html") == TextFormat::Html);
	CHECK(!TextFormatNamed("bbcode") && !TextFormatNamed(""));
	CHECK(Utf16Length("abc") == 3);
	CHECK(Utf16Length("\xD0\xB0\xD0\xB1") == 2);
	CHECK(Utf16Length("\xE2\x82\xAC") == 1);
	CHECK(Utf16Length("\xF0\x9F\x98\x80") == 2);
}

void TestRandomInput() {
	static const char kAlphabet[] = "*_~|`[]()!\\<>&;/=\"' ab1#:\n-";
	auto state = uint32_t(12345);
	const auto next = [&] {
		state = state * 1664525u + 1013904223u;
		return state >> 8;
	};
	for (auto round = 0; round != 30000; ++round) {
		auto source = std::string();
		const auto length = next() % 40;
		for (auto i = uint32_t(0); i != length; ++i) {
			source += kAlphabet[next() % (sizeof(kAlphabet) - 1)];
		}
		for (const auto format : { TextFormat::Markdown, TextFormat::Html }) {
			const auto parsed = ParseMarkup(format, source);
			if (!parsed.ok()) {
				continue;
			}
			const auto units = Utf16Length(parsed.value().text);
			auto previous = int64_t(-1);
			for (const auto &entity : parsed.value().entities) {
				CHECK(entity.offset >= 0 && entity.length > 0);
				CHECK(entity.offset + entity.length <= units);
				CHECK(entity.offset >= previous);
				CHECK(KnownEntityType(entity.type));
				previous = entity.offset;
			}
			CHECK(parsed.value().entities.size() <= kMaxTextEntities);
		}
	}
}

} // namespace

int main() {
	TestBasics();
	TestMarkdown();
	TestHtml();
	TestRandomInput();
	return Failures() ? 1 : 0;
}
