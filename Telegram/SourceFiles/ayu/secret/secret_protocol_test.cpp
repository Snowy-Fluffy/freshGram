// Standalone regression test for the secret-chat re-keying wire format.
//
// Covers the PFS control messages (RequestKey / AcceptKey / CommitKey /
// AbortKey): builder output must round-trip through ParseMessage with all
// fields intact, constructor tags must match
// https://core.telegram.org/api/end-to-end/pfs, and truncated input must
// be rejected instead of producing a half-parsed action.
//
// The state machine itself (Manager::Impl) needs a Main::Session and is
// verified by the manual matrix in the PR description; this test guards
// the serialization layer it is built on.
//
// Build (no Qt, no full client build needed):
//   g++ -std=c++20 -Wall -Wextra -I Telegram/SourceFiles
//     Telegram/SourceFiles/ayu/secret/secret_protocol.cpp
//     Telegram/SourceFiles/ayu/secret/secret_model.cpp
//     Telegram/SourceFiles/ayu/secret/secret_crypto.cpp
//     Telegram/SourceFiles/ayu/secret/secret_protocol_test.cpp
//     -lcrypto -o /tmp/secret_protocol_test && /tmp/secret_protocol_test

#include "ayu/secret/secret_protocol.h"
#include "ayu/secret/secret_tl.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int Failures = 0;
int Checks = 0;

void Check(bool condition, const char *name) {
	++Checks;
	if (!condition) {
		++Failures;
		std::printf("FAIL: %s\n", name);
	} else {
		std::printf("ok: %s\n", name);
	}
}

uint32_t ReadTag(const AyuSecret::Bytes &bytes, size_t offset) {
	uint32_t tag = 0;
	std::memcpy(&tag, bytes.data() + offset, sizeof(tag));
	return tag;
}

AyuSecret::Bytes Filled(size_t size, uint8_t byte) {
	return AyuSecret::Bytes(size, byte);
}

bool ParseService(const AyuSecret::Bytes &bytes, AyuSecret::Inbound &out) {
	const auto layer = AyuSecret::BuildLayerObject(bytes, 0, 0);
	return AyuSecret::ParseLayerObject(layer, out);
}

void TestRequestKey() {
	const int64_t randomId = 0x1122334455667788LL;
	const int64_t exchangeId = -0x123456789ABCDEFLL; // ids are signed longs
	const auto gA = Filled(256, 0xA5);
	const auto bytes = AyuSecret::BuildRequestKey(randomId, exchangeId, gA);
	Check(ReadTag(bytes, 12) == 0xf3c9611bU, "request tag matches spec");
	auto inbound = AyuSecret::Inbound();
	Check(ParseService(bytes, inbound), "request parses");
	Check(inbound.service, "request is a service message");
	Check(inbound.randomId == randomId, "request random id round-trips");
	Check(
		inbound.action == AyuSecret::ActionKind::RequestKey,
		"request action kind");
	Check(inbound.exchangeId == exchangeId, "request exchange id round-trips");
	Check(inbound.value == gA, "request g_a round-trips");
}

void TestAcceptKey() {
	const int64_t randomId = 42;
	const int64_t exchangeId = 0x7FFFFFFFFFFFFFFFLL;
	const auto gB = Filled(256, 0x3C);
	const int64_t fingerprint = -0x77AA55CC33EE11LL;
	const auto bytes = AyuSecret::BuildAcceptKey(
		randomId,
		exchangeId,
		gB,
		fingerprint);
	Check(ReadTag(bytes, 12) == 0x6fe1735bU, "accept tag matches spec");
	auto inbound = AyuSecret::Inbound();
	Check(ParseService(bytes, inbound), "accept parses");
	Check(
		inbound.action == AyuSecret::ActionKind::AcceptKey,
		"accept action kind");
	Check(inbound.exchangeId == exchangeId, "accept exchange id round-trips");
	Check(inbound.value == gB, "accept g_b round-trips");
	Check(inbound.fingerprint == fingerprint, "accept fingerprint round-trips");
}

void TestCommitKey() {
	const int64_t exchangeId = 7;
	const int64_t fingerprint = -1;
	const auto bytes = AyuSecret::BuildCommitKey(99, exchangeId, fingerprint);
	Check(ReadTag(bytes, 12) == 0xec2e0b9bU, "commit tag matches spec");
	auto inbound = AyuSecret::Inbound();
	Check(ParseService(bytes, inbound), "commit parses");
	Check(
		inbound.action == AyuSecret::ActionKind::CommitKey,
		"commit action kind");
	Check(inbound.exchangeId == exchangeId, "commit exchange id round-trips");
	Check(inbound.fingerprint == fingerprint, "commit fingerprint round-trips");
	Check(inbound.value.empty(), "commit carries no key material");
}

void TestAbortKey() {
	const int64_t exchangeId = 0x0102030405060708LL;
	const auto bytes = AyuSecret::BuildAbortKey(1000, exchangeId);
	Check(ReadTag(bytes, 12) == 0xdd05ec6bU, "abort tag matches spec");
	auto inbound = AyuSecret::Inbound();
	Check(ParseService(bytes, inbound), "abort parses");
	Check(
		inbound.action == AyuSecret::ActionKind::AbortKey,
		"abort action kind");
	Check(inbound.exchangeId == exchangeId, "abort exchange id round-trips");
}

void TestTruncated() {
	const auto gA = Filled(256, 0xA5);
	const auto full = AyuSecret::BuildRequestKey(1, 2, gA);
	for (const auto cut : { size_t(1), size_t(11), size_t(13) }) {
		auto inbound = AyuSecret::Inbound();
		const auto chopped = AyuSecret::Bytes(
			full.begin(),
			full.end() - cut);
		Check(
			!ParseService(chopped, inbound),
			"truncated request is rejected");
	}
	{
		// A truncated transport envelope is rejected as well.
		const auto layer = AyuSecret::BuildLayerObject(full, 0, 0);
		auto inbound = AyuSecret::Inbound();
		const auto chopped = AyuSecret::Bytes(
			layer.begin(),
			layer.end() - 1);
		Check(
			!AyuSecret::ParseLayerObject(chopped, inbound),
			"truncated layer is rejected");
	}
	{
		auto inbound = AyuSecret::Inbound();
		const auto empty = AyuSecret::Bytes();
		Check(
			!AyuSecret::ParseLayerObject(empty, inbound),
			"empty input is rejected");
	}
	{
		// Unknown action constructor must not be mistaken for a key action.
		auto noop = AyuSecret::BuildNoop(5);
		const uint32_t bogus = 0xFFFFFFFFU;
		std::memcpy(noop.data() + 12, &bogus, sizeof(bogus));
		auto inbound = AyuSecret::Inbound();
		Check(
			ParseService(noop, inbound)
				&& inbound.action == AyuSecret::ActionKind::Other,
			"unknown action decodes as Other, not as a key action");
	}
}

} // namespace

int main() {
	TestRequestKey();
	TestAcceptKey();
	TestCommitKey();
	TestAbortKey();
	TestTruncated();
	std::printf(
		"%d checks, %d failures\n",
		Checks,
		Failures);
	return Failures ? 1 : 0;
}
