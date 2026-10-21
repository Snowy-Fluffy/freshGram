#include "check.h"
#include "cbor.h"

#include <cmath>
#include <limits>

namespace {

using namespace fgpl::cbor;

[[nodiscard]] Bytes FromHex(std::string_view hex) {
	auto result = Bytes();
	const auto digit = [](char ch) {
		return (ch <= '9') ? (ch - '0') : (ch - 'a' + 10);
	};
	for (auto i = size_t(0); i + 1 < hex.size(); i += 2) {
		result.push_back(uint8_t(digit(hex[i]) * 16 + digit(hex[i + 1])));
	}
	return result;
}

[[nodiscard]] Value Text(std::string_view text) {
	return Value::FromText(std::string(text));
}

[[nodiscard]] Value Int(int64_t number) {
	return Value::FromInt(number);
}

void CheckRoundTrip(std::string_view hex, const Value &value) {
	const auto bytes = FromHex(hex);
	const auto decoded = Decode(bytes);
	CHECK(decoded.ok());
	if (decoded.ok()) {
		CHECK(decoded.value() == value);
	}
	auto encoded = Bytes();
	CHECK(Encode(value, encoded));
	CHECK(encoded == bytes);
}

void CheckDecodeFloat(std::string_view hex, double expected) {
	const auto decoded = Decode(FromHex(hex));
	CHECK(decoded.ok());
	if (decoded.ok()) {
		const auto number = decoded.value().asFloat();
		CHECK(number.has_value());
		if (number) {
			if (std::isnan(expected)) {
				CHECK(std::isnan(*number));
			} else {
				CHECK(*number == expected);
			}
		}
	}
}

void CheckError(std::string_view hex, Error expected) {
	const auto decoded = Decode(FromHex(hex));
	CHECK(!decoded.ok());
	if (!decoded.ok()) {
		CHECK(decoded.error() == expected);
	}
}

void TestIntegers() {
	CheckRoundTrip("00", Int(0));
	CheckRoundTrip("01", Int(1));
	CheckRoundTrip("0a", Int(10));
	CheckRoundTrip("17", Int(23));
	CheckRoundTrip("1818", Int(24));
	CheckRoundTrip("1819", Int(25));
	CheckRoundTrip("1864", Int(100));
	CheckRoundTrip("1903e8", Int(1000));
	CheckRoundTrip("1a000f4240", Int(1000000));
	CheckRoundTrip("1b000000e8d4a51000", Int(1000000000000));
	CheckRoundTrip("20", Int(-1));
	CheckRoundTrip("29", Int(-10));
	CheckRoundTrip("3863", Int(-100));
	CheckRoundTrip("3903e7", Int(-1000));
	CheckRoundTrip(
		"1b7fffffffffffffff",
		Int(std::numeric_limits<int64_t>::max()));
	CheckRoundTrip(
		"3b7fffffffffffffff",
		Int(std::numeric_limits<int64_t>::min()));
}

void TestIntegerRange() {
	CheckError("1b8000000000000000", Error::IntRange);
	CheckError("1bffffffffffffffff", Error::IntRange);
	CheckError("3b8000000000000000", Error::IntRange);
}

void TestSimple() {
	CheckRoundTrip("f4", Value::FromBool(false));
	CheckRoundTrip("f5", Value::FromBool(true));
	CheckRoundTrip("f6", Value());
	CheckRoundTrip(
		"fb3ff199999999999a",
		Value::FromFloat(1.1));
	CheckRoundTrip(
		"fbc010666666666666",
		Value::FromFloat(-4.1));
}

void TestFloatWidths() {
	CheckDecodeFloat("f93c00", 1.);
	CheckDecodeFloat("f93e00", 1.5);
	CheckDecodeFloat("f97bff", 65504.);
	CheckDecodeFloat("f90001", std::ldexp(1., -24));
	CheckDecodeFloat("f9c400", -4.);
	CheckDecodeFloat("f97c00", std::numeric_limits<double>::infinity());
	CheckDecodeFloat("f9fc00", -std::numeric_limits<double>::infinity());
	CheckDecodeFloat("f97e00", std::numeric_limits<double>::quiet_NaN());
	CheckDecodeFloat("fa47c35000", 100000.);
	CheckDecodeFloat("fa7f800000", std::numeric_limits<double>::infinity());
	CheckDecodeFloat("fb7ff0000000000000", std::numeric_limits<double>::infinity());
}

void TestStrings() {
	CheckRoundTrip("60", Text(""));
	CheckRoundTrip("6161", Text("a"));
	CheckRoundTrip("6449455446", Text("IETF"));
	CheckRoundTrip("62225c", Text("\"\\"));
	CheckRoundTrip("62c3bc", Text("\xC3\xBC"));
	CheckRoundTrip("63e6b0b4", Text("\xE6\xB0\xB4"));
	CheckRoundTrip("64f0908591", Text("\xF0\x90\x85\x91"));
	CheckRoundTrip("40", Value::FromBytes({}));
	CheckRoundTrip("4401020304", Value::FromBytes({ 1, 2, 3, 4 }));
}

void TestContainers() {
	CheckRoundTrip("80", Value::FromArray({}));
	CheckRoundTrip(
		"83010203",
		Value::FromArray({ Int(1), Int(2), Int(3) }));
	CheckRoundTrip(
		"8301820203820405",
		Value::FromArray({
			Int(1),
			Value::FromArray({ Int(2), Int(3) }),
			Value::FromArray({ Int(4), Int(5) }),
		}));
	CheckRoundTrip("a0", Value::FromMap({}));
	CheckRoundTrip(
		"a26161016162820203",
		Value::FromMap({
			{ "a", Int(1) },
			{ "b", Value::FromArray({ Int(2), Int(3) }) },
		}));
}

void TestLookup() {
	const auto decoded = Decode(FromHex("a26161016162f5"));
	CHECK(decoded.ok());
	if (!decoded.ok()) {
		return;
	}
	const auto &map = decoded.value();
	CHECK(map.find("a") && map.find("a")->asInt() == 1);
	CHECK(map.find("b") && map.find("b")->asBool() == true);
	CHECK(map.find("c") == nullptr);
	CHECK(Int(1).find("a") == nullptr);
}

void TestRejections() {
	CheckError("", Error::Truncated);
	CheckError("18", Error::Truncated);
	CheckError("1b0000", Error::Truncated);
	CheckError("62c3", Error::Truncated);
	CheckError("83010203" "00", Error::TrailingData);
	CheckError("1c", Error::Malformed);
	CheckError("1f", Error::Malformed);
	CheckError("3f", Error::Malformed);
	CheckError("ff", Error::Malformed);
	CheckError("df", Error::Malformed);
	CheckError("5f", Error::Unsupported);
	CheckError("7f", Error::Unsupported);
	CheckError("9f", Error::Unsupported);
	CheckError("bf", Error::Unsupported);
	CheckError("c074323031332d30332d32315432303a30343a30305a", Error::Unsupported);
	CheckError("f7", Error::Unsupported);
	CheckError("f0", Error::Unsupported);
	CheckError("f818", Error::Unsupported);
	CheckError("62c328", Error::BadUtf8);
	CheckError("62c080", Error::BadUtf8);
	CheckError("63eda080", Error::BadUtf8);
	CheckError("64f4908080", Error::BadUtf8);
	CheckError("61ff", Error::BadUtf8);
	CheckError("a1616101616102", Error::TrailingData);
	CheckError("a2616101616102", Error::DuplicateKey);
	CheckError("a10102", Error::KeyNotText);
	CheckError("a1", Error::Truncated);
	CheckError("a2616101", Error::Truncated);
}

void TestCountsAreBoundedByInput() {
	CheckError("9bffffffffffffffff", Error::TooLarge);
	CheckError("9a00ffffff", Error::TooLarge);
	CheckError("9902ff", Error::Truncated);
	CheckError("5bffffffffffffffff", Error::TooLarge);
	CheckError("5a00100001", Error::TooLarge);
	CheckError("5a00000010" "00", Error::Truncated);
}

void TestLimits() {
	auto deep = Bytes(40, 0x81);
	deep.push_back(0x00);
	CHECK(!Decode(deep).ok() && Decode(deep).error() == Error::TooDeep);

	auto limits = Limits();
	limits.maxDepth = 64;
	CHECK(Decode(deep, limits).ok());

	auto many = Bytes{ 0x98, 0x05, 0, 0, 0, 0, 0 };
	limits = Limits();
	limits.maxItems = 5;
	CHECK(!Decode(many, limits).ok()
		&& Decode(many, limits).error() == Error::TooManyItems);
	limits.maxItems = 6;
	CHECK(Decode(many, limits).ok());

	limits = Limits();
	limits.maxContainer = 2;
	CHECK(!Decode(FromHex("83010203"), limits).ok()
		&& Decode(FromHex("83010203"), limits).error() == Error::TooLarge);

	limits = Limits();
	limits.maxString = 3;
	CHECK(Decode(FromHex("63616263"), limits).ok());
	CHECK(!Decode(FromHex("6461626364"), limits).ok()
		&& Decode(FromHex("6461626364"), limits).error() == Error::TooLarge);
}

void TestPrefix() {
	const auto bytes = FromHex("8301020300");
	const auto decoded = DecodePrefix(bytes);
	CHECK(decoded.ok());
	if (decoded.ok()) {
		CHECK(decoded.value().used == 4);
	}
}

void TestEncodeRejections() {
	auto out = Bytes{ 0xAA };
	CHECK(!Encode(Text("bad\xFF"), out));
	CHECK(out == Bytes{ 0xAA });

	auto nested = Value();
	for (auto i = 0; i != 40; ++i) {
		nested = Value::FromArray({ nested });
	}
	CHECK(!Encode(nested, out));
	CHECK(out == Bytes{ 0xAA });
	CHECK(Encode(nested, out, 64));

	auto badKey = Value::FromMap({ { "k\xFF", Int(1) } });
	out = {};
	CHECK(!Encode(badKey, out));
}

void TestIsValidUtf8() {
	CHECK(IsValidUtf8(""));
	CHECK(IsValidUtf8("plain ascii"));
	CHECK(IsValidUtf8("\xC3\xBC\xE6\xB0\xB4\xF0\x90\x85\x91"));
	CHECK(!IsValidUtf8("\xC0\xAF"));
	CHECK(!IsValidUtf8("\xE0\x80\xAF"));
	CHECK(!IsValidUtf8("\xED\xA0\x80"));
	CHECK(!IsValidUtf8("\xF4\x90\x80\x80"));
	CHECK(!IsValidUtf8("\xF8\x88\x80\x80\x80"));
	CHECK(!IsValidUtf8("\x80"));
	CHECK(!IsValidUtf8("\xE6\xB0"));
}

} // namespace

int main() {
	TestIntegers();
	TestIntegerRange();
	TestSimple();
	TestFloatWidths();
	TestStrings();
	TestContainers();
	TestLookup();
	TestRejections();
	TestCountsAreBoundedByInput();
	TestLimits();
	TestPrefix();
	TestEncodeRejections();
	TestIsValidUtf8();
	return Failures() ? 1 : 0;
}
