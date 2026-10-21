#include "fgpl/base64.h"
#include "check.h"
#include "fgpl/json.h"
#include "fgpl/semver.h"

#include <string>
#include <vector>

namespace {

using namespace fgpl;

[[nodiscard]] std::vector<uint8_t> Bytes(std::string_view text) {
	return { text.begin(), text.end() };
}

void TestBase64() {
	CHECK(Base64Encode(Bytes("")) == "");
	CHECK(Base64Encode(Bytes("f")) == "Zg==");
	CHECK(Base64Encode(Bytes("fo")) == "Zm8=");
	CHECK(Base64Encode(Bytes("foo")) == "Zm9v");
	CHECK(Base64Encode(Bytes("foobar")) == "Zm9vYmFy");
	CHECK(Base64Decode("Zm9vYmFy") == Bytes("foobar"));
	CHECK(Base64Decode("Zm8=") == Bytes("fo"));
	CHECK(Base64Decode("Zg==") == Bytes("f"));
	CHECK(Base64Decode("") == Bytes(""));
	for (const auto bad : {
		"Zg=", "Zg", "Zg===", "=Zg=", "Zm9v!", "Zm 9v", "Zg=a", "Zh==", "Zm9=",
		"Zm9vYmFy\n", "Z===",
	}) {
		if (Base64Decode(bad)) {
			std::fprintf(stderr, "accepted: %s\n", bad);
			CHECK(false);
		}
	}
	auto all = std::vector<uint8_t>();
	for (auto i = 0; i != 256; ++i) {
		all.push_back(uint8_t(i));
	}
	CHECK(Base64Decode(Base64Encode(all)) == all);
}

void TestJsonParse() {
	const auto value = json::Parse(R"({"a": [1, -2, true, null, "x\n\u00e9\ud83d\ude00"], "b": {"c": 0}})");
	CHECK(value.ok());
	if (value.ok()) {
		const auto list = value.value().find("a")->asArray();
		CHECK(list && list->size() == 5);
		CHECK(list && (*list)[1].asInt() == -2);
		CHECK(list && *(*list)[4].asText() == "x\n\xC3\xA9\xF0\x9F\x98\x80");
		CHECK(value.value().find("b")->find("c")->asInt() == 0);
	}
	CHECK(json::Parse("  {}  ").ok());
	CHECK(json::Parse("[]").ok());
	for (const auto bad : {
		"", "{", "[1,]", "{\"a\":1,}", "{\"a\" 1}", "{a:1}", "[1 2]",
		"{\"a\":1} x", "01", "-", "1.5", "1e3", "-0.0", "\"\\x\"", "\"\\u12\"",
		"\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0041\"", "\"a\nb\"",
		"{\"a\":1,\"a\":2}", "'single'", "nan", "True", "[", "}", "{\"a\":}",
		"9007199254740992", "-9007199254740992",
		"\xEF\xBB\xBF{}", "\"\xC3\x28\"",
	}) {
		if (json::Parse(bad).ok()) {
			std::fprintf(stderr, "accepted: %s\n", bad);
			CHECK(false);
		}
	}
	CHECK(json::Parse("9007199254740991").ok());
	auto deep = std::string(40, '[') + std::string(40, ']');
	CHECK(!json::Parse(deep).ok());
	auto limits = json::Limits();
	limits.maxBytes = 10;
	CHECK(!json::Parse("[1,2,3,4,5,6,7,8]", limits).ok());
	limits = json::Limits();
	limits.maxItems = 5;
	CHECK(!json::Parse("[1,2,3,4,5,6,7,8]", limits).ok());
}

void TestCanonicalJson() {
	const auto parsed = json::Parse(R"({"b": 2, "a": {"z": 1, "y": [3, "x"]}, "c": "q\"\\/\u0001\u007f"})");
	CHECK(parsed.ok());
	if (parsed.ok()) {
		CHECK(json::Canonical(parsed.value())
			== "{\"a\":{\"y\":[3,\"x\"],\"z\":1},\"b\":2,\"c\":\"q\\\"\\\\/\\u0001\x7f\"}");
	}
	const auto unicode = json::Parse("{\"\\ue000\": 1, \"\\ud83d\\ude00\": 2, \"a\": 3}");
	CHECK(unicode.ok());
	if (unicode.ok()) {
		CHECK(json::Canonical(unicode.value())
			== "{\"a\":3,\"\xF0\x9F\x98\x80\":2,\"\xEE\x80\x80\":1}");
	}
	const auto reordered = json::Parse(R"({"a":1,"b":2})");
	const auto original = json::Parse(R"({  "b" : 2 , "a" : 1 })");
	CHECK(json::Canonical(reordered.value()) == json::Canonical(original.value()));
	CHECK(json::Canonical(cbor::Value::FromMap({})) == "{}");
	CHECK(json::Canonical(json::Parse("-5").value()) == "-5");
}

void TestVersions() {
	const auto v = [](const char *text) { return ParseVersion(text).value(); };
	CHECK(ParseVersion("1.2.3").has_value());
	CHECK(ParseVersion("7.5.9-beta.1").has_value());
	for (const auto bad : {
		"1", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "a.b.c", "1.2.x", "",
		"1.2.3-", "1.2.3-a..b", "1.2.3-.a", "-1.2.3", "1.2.3 ", " 1.2.3",
		"1.2.99999999999",
	}) {
		if (ParseVersion(bad)) {
			std::fprintf(stderr, "accepted: %s\n", bad);
			CHECK(false);
		}
	}
	CHECK(Compare(v("1.2.3"), v("1.2.3")) == 0);
	CHECK(Compare(v("1.2.3"), v("1.2.4")) < 0);
	CHECK(Compare(v("1.10.0"), v("1.9.0")) > 0);
	CHECK(Compare(v("2.0.0"), v("1.99.99")) > 0);
	CHECK(Compare(v("1.0.0-alpha"), v("1.0.0")) < 0);
	CHECK(Compare(v("1.0.0-alpha"), v("1.0.0-alpha.1")) < 0);
	CHECK(Compare(v("1.0.0-alpha.1"), v("1.0.0-alpha.beta")) < 0);
	CHECK(Compare(v("1.0.0-beta"), v("1.0.0-beta.2")) < 0);
	CHECK(Compare(v("1.0.0-beta.2"), v("1.0.0-beta.11")) < 0);
	CHECK(Compare(v("1.0.0-rc.1"), v("1.0.0")) < 0);
	CHECK(v("7.5.9-beta.1").text() == "7.5.9-beta.1");
}

void TestRanges() {
	const auto range = [](const char *text) {
		return VersionRange::Parse(text).value();
	};
	const auto v = [](const char *text) { return ParseVersion(text).value(); };
	const auto window = range(">=7.6.0 <8.0.0");
	CHECK(window.matches(v("7.6.0")));
	CHECK(window.matches(v("7.99.1")));
	CHECK(!window.matches(v("7.5.9")));
	CHECK(!window.matches(v("8.0.0")));
	CHECK(!window.matches(v("7.6.0-beta")));
	CHECK(range("*").matches(v("0.0.1")));
	CHECK(range("1.2.3").matches(v("1.2.3")));
	CHECK(!range("1.2.3").matches(v("1.2.4")));
	CHECK(range("=1.2.3").matches(v("1.2.3")));
	CHECK(range(">1.0.0").matches(v("1.0.1")));
	CHECK(!range(">1.0.0").matches(v("1.0.0")));
	CHECK(range("<=1.0.0").matches(v("1.0.0")));
	CHECK(range("  >=1.0.0   <2.0.0 ").matches(v("1.5.0")));
	for (const auto bad : { "", "   ", ">=", ">=1", "^1.2.3", "~1.2.3", "1.x", ">= 1.0.0", "1.0.0 ||2.0.0", ">=1.0.0 <2.0.0 >=0.0.1 >=0.0.2 >=0.0.3 >=0.0.4 >=0.0.5 >=0.0.6 >=0.0.7" }) {
		if (VersionRange::Parse(bad)) {
			std::fprintf(stderr, "accepted: '%s'\n", bad);
			CHECK(false);
		}
	}
}

} // namespace

int main() {
	TestBase64();
	TestJsonParse();
	TestCanonicalJson();
	TestVersions();
	TestRanges();
	return Failures() ? 1 : 0;
}
