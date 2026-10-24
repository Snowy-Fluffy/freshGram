// Standalone unit test for the PFS decision logic (secret_rekey.h).
// Pure functions, no Qt, no session, no crypto needed.
//
// Build (no full client build needed):
//   g++ -std=c++20 -Wall -Wextra -I Telegram/SourceFiles
//     Telegram/SourceFiles/ayu/secret/secret_rekey_test.cpp
//     -o /tmp/secret_rekey_test && /tmp/secret_rekey_test

#include "ayu/secret/secret_rekey.h"

#include <cstdint>
#include <cstdio>
#include <limits>

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

using FreshSecret::ConcurrentDecision;
using FreshSecret::DecideConcurrent;
using FreshSecret::ReleaseIncomingExchange;
using FreshSecret::ShouldRekey;
using FreshSecret::SupportsRekey;

constexpr auto kUses = 100;
constexpr auto kUsesIn = 120;
constexpr auto kTime = 7 * 86400;

void TestConcurrent() {
	Check(
		DecideConcurrent(5, 3) == ConcurrentDecision::IgnoreIncoming,
		"larger ours wins");
	Check(
		DecideConcurrent(3, 5) == ConcurrentDecision::AnswerIncoming,
		"larger incoming wins");
	Check(
		DecideConcurrent(7, 7) == ConcurrentDecision::AbortBoth,
		"equal ids abort both");
	// Signed-long comparison, including negatives (RandomId is signed).
	Check(
		DecideConcurrent(-1, -2) == ConcurrentDecision::IgnoreIncoming,
		"signed compare: -1 > -2");
	Check(
		DecideConcurrent(-2, -1) == ConcurrentDecision::AnswerIncoming,
		"signed compare: -2 < -1");
	Check(
		DecideConcurrent(-5, 5) == ConcurrentDecision::AnswerIncoming,
		"signed compare: negative < positive");
	Check(
		DecideConcurrent(
			std::numeric_limits<int64_t>::min(),
			std::numeric_limits<int64_t>::max())
			== ConcurrentDecision::AnswerIncoming,
		"extreme ids compare as signed");
}

void TestTrigger() {
	const auto now = 1'800'000'000;
	Check(
		!ShouldRekey(99, 119, now - 10, now, kUses, kUsesIn, kTime),
		"below both directional limits");
	Check(
		ShouldRekey(100, 0, now - 10, now, kUses, kUsesIn, kTime),
		"100 encryptions trigger");
	Check(
		ShouldRekey(0, 120, now - 10, now, kUses, kUsesIn, kTime),
		"120 decryptions trigger");
	Check(
		ShouldRekey(1, 0, now - kTime - 1, now, kUses, kUsesIn, kTime),
		"old key with one encryption triggers");
	Check(
		ShouldRekey(0, 200, now - kTime - 1, now, kUses, kUsesIn, kTime),
		"200 decryptions trigger even without encryptions");
	Check(
		!ShouldRekey(0, 50, now - kTime - 1, now, kUses, kUsesIn, kTime),
		"old key without encryptions and under count does not trigger");
	Check(
		!ShouldRekey(1, 0, now - 10, now, kUses, kUsesIn, kTime),
		"fresh key does not trigger");
	Check(
		!ShouldRekey(0, 0, now - 10, now, kUses, kUsesIn, kTime),
		"unused key does not trigger");
}

void TestDhFailure() {
	auto exchange = int64_t(17);
	Check(ReleaseIncomingExchange(exchange, 17), "DH failure releases its exchange");
	Check(exchange == 0, "RequestKey can reserve again after failure");
	exchange = 17;
	Check(ReleaseIncomingExchange(exchange, 17), "retried RequestKey can fail again");
	exchange = 23;
	Check(!ReleaseIncomingExchange(exchange, 17), "stale failure ignores a new exchange");
	Check(exchange == 23, "new exchange survives stale failure");
	Check(!ReleaseIncomingExchange(exchange, 0), "zero id cannot release a reservation");
}

void TestLayers() {
	Check(!SupportsRekey(0), "unknown peer layer cannot initiate PFS");
	Check(!SupportsRekey(45), "legacy peer cannot initiate PFS");
	Check(SupportsRekey(46), "PFS threshold is supported");
	Check(SupportsRekey(144), "current peer supports PFS");
}

} // namespace

int main() {
	TestConcurrent();
	TestTrigger();
	TestDhFailure();
	TestLayers();
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
