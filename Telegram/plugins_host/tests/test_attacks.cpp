#include "check.h"
#include "host_client.h"

#include <signal.h>
#include <sys/resource.h>

#include <chrono>
#include <string>
#include <vector>

namespace {

using namespace fgpl;

class AttackBridge final : public Bridge {
public:
	cbor::Value call(const cbor::Value &) override {
		++calls;
		return cbor::Value::FromText(std::string(replySize, 'r'));
	}
	void log(std::string_view line) override {
		logBytes += line.size() + 1;
	}

	size_t replySize = 8;
	size_t calls = 0;
	size_t logBytes = 0;

};

enum class Stage : uint8_t {
	Start,
	Load,
	Event,
};

struct Outcome {
	bool succeeded = false;
	Stage stage = Stage::Start;
	ClientFailure failure;
	double seconds = 0;
	size_t logBytes = 0;
	size_t calls = 0;
	bool deadAfter = false;
};

[[nodiscard]] Outcome Run(const std::string &attack, size_t replySize = 8) {
	auto outcome = Outcome();
	const auto begin = std::chrono::steady_clock::now();
	const auto finish = [&] {
		outcome.seconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - begin).count();
	};

	auto config = HostConfig();
	config.path = FGPL_EVIL_HOST;
	config.startupTimeoutMs = 500;
	config.sendTimeoutMs = 500;
	config.addressSpaceMb = 0;
	config.reservationMb = 0;
	config.extraArguments = { "--attack=" + attack };
	auto started = HostClient::Start(config);
	if (!started.ok()) {
		outcome.stage = Stage::Start;
		outcome.failure = started.error();
		finish();
		return outcome;
	}
	auto &client = started.value();

	auto request = LoadRequest();
	request.limits.callTimeoutMs = 100;
	const auto loaded = client->load(request);
	if (!loaded.ok()) {
		outcome.stage = Stage::Load;
		outcome.failure = loaded.error();
		finish();
		return outcome;
	}

	auto bridge = AttackBridge();
	bridge.replySize = replySize;
	const auto result = client->deliver(cbor::Value::FromInt(1), bridge);
	outcome.stage = Stage::Event;
	outcome.succeeded = result.ok();
	if (!result.ok()) {
		outcome.failure = result.error();
	}
	outcome.logBytes = bridge.logBytes;
	outcome.calls = bridge.calls;
	const auto after = client->deliver(cbor::Value::FromInt(2), bridge);
	outcome.deadAfter = !after.ok()
		&& after.error().error == ClientError::HostDied;
	finish();
	return outcome;
}

struct Case {
	const char *attack;
	Stage stage;
	ClientError error;
	double maxSeconds;
	int signal = 0;
};

void Expect(const Case &item, size_t replySize = 8) {
	const auto outcome = Run(item.attack, replySize);
	const auto fail = [&](const char *why) {
		std::fprintf(
			stderr,
			"%s: %s (stage %d, error %d, signal %d, exit %d, %.2f s, detail '%s')\n",
			item.attack,
			why,
			int(outcome.stage),
			int(outcome.failure.error),
			outcome.failure.signal,
			outcome.failure.exitCode,
			outcome.seconds,
			outcome.failure.detail.c_str());
		CHECK(false);
	};
	if (outcome.succeeded) {
		fail("expected a failure");
	} else if (outcome.stage != item.stage) {
		fail("failed at a different stage");
	} else if (outcome.failure.error != item.error) {
		fail("a different error");
	} else if (item.signal != 0 && outcome.failure.signal != item.signal) {
		fail("a different signal");
	} else if (outcome.seconds > item.maxSeconds) {
		fail("took too long");
	}
}

void TestStartupAttacks() {
	const auto protocol = ClientError::Protocol;
	Expect({ "no_hello", Stage::Start, ClientError::Timeout, 2 });
	Expect({ "hello_garbage", Stage::Start, protocol, 2 });
	Expect({ "hello_wrong_type", Stage::Start, protocol, 2 });
	Expect({ "hello_giant_length", Stage::Start, protocol, 2 });
	Expect({ "hello_stall", Stage::Start, ClientError::Timeout, 2 });
	Expect({ "hello_crash", Stage::Start, ClientError::HostDied, 2, SIGSEGV });
}

void TestLoadAttacks() {
	const auto protocol = ClientError::Protocol;
	for (const auto attack : {
		"load_giant_length", "load_over_limit", "load_garbage", "load_deep",
		"load_big_count", "load_many_items", "load_huge_string",
		"load_bad_utf8", "load_duplicate_keys", "load_wrong_reply",
	}) {
		Expect({ attack, Stage::Load, protocol, 4 });
	}
	Expect({ "load_crash", Stage::Load, ClientError::HostDied, 4, SIGSEGV });
	Expect({ "load_abort", Stage::Load, ClientError::HostDied, 4, SIGABRT });
	Expect({ "load_exit", Stage::Load, ClientError::HostDied, 4 });
	Expect({ "hello_then_exit", Stage::Load, ClientError::HostDied, 4 });
}

void TestEventAttacks() {
	const auto protocol = ClientError::Protocol;
	Expect({ "event_call_flood", Stage::Event, ClientError::HostDied, 4 });
	Expect({ "event_call_huge_request", Stage::Event, protocol, 4 });
	Expect({ "event_wrong_id", Stage::Event, protocol, 4 });
	Expect({ "event_unsolicited", Stage::Event, protocol, 4 });
	Expect({ "event_huge_detail", Stage::Event, protocol, 4 });
	Expect({ "event_garbage", Stage::Event, protocol, 4 });
	Expect({ "event_slowloris", Stage::Event, ClientError::Timeout, 8 });
	Expect({ "event_stuck", Stage::Event, ClientError::Timeout, 8 });
	Expect({ "event_crash", Stage::Event, ClientError::HostDied, 4, SIGSEGV });
	Expect({ "event_exit", Stage::Event, ClientError::HostDied, 4 });
	Expect({ "event_log_forever", Stage::Event, ClientError::Timeout, 8 });
	Expect(
		{ "event_call_unread_replies", Stage::Event, ClientError::HostDied, 8 },
		size_t(1) << 20);
}

void TestLogFloodIsCapped() {
	const auto outcome = Run("event_log_flood");
	CHECK(outcome.succeeded);
	CHECK(outcome.logBytes <= (size_t(1) << 20));
	CHECK(outcome.logBytes > 0);
	CHECK(outcome.seconds < 10);
	const auto forever = Run("event_log_forever");
	CHECK(forever.logBytes <= (size_t(1) << 20));
}

void TestEveryFailureLeavesTheClientDead() {
	for (const auto attack : {
		"event_wrong_id", "event_crash", "event_call_flood", "event_stuck",
	}) {
		const auto outcome = Run(attack);
		if (!outcome.deadAfter) {
			std::fprintf(stderr, "%s: the client kept using a bad host\n", attack);
			CHECK(false);
		}
	}
}

void TestMemoryStaysBounded() {
#ifndef FGPL_SANITIZED
	auto usage = rusage();
	getrusage(RUSAGE_SELF, &usage);
	CHECK(usage.ru_maxrss < 400 * 1024);
#endif
}

} // namespace

int main() {
	TestStartupAttacks();
	TestLoadAttacks();
	TestEventAttacks();
	TestLogFloodIsCapped();
	TestEveryFailureLeavesTheClientDead();
	TestMemoryStaysBounded();
	return Failures() ? 1 : 0;
}
