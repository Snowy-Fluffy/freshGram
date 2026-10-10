#pragma once

#include <cstdint>

namespace AyuSecret {

// Pure decision logic of the PFS re-keying state machine, see
// https://core.telegram.org/api/end-to-end/pfs
// Kept free of Qt/session dependencies so it is unit-testable standalone
// (secret_rekey_test.cpp). The Manager implements the I/O around these.

// Concurrent initiation arbitration: when both sides sent RequestKey,
// only the instance with the larger exchange id survives. Exchange ids
// are compared as signed longs; an exact match (2^-64) aborts both.
enum class ConcurrentDecision {
	// Our instance wins: ignore the incoming one silently, the other
	// side applies the same rule and drops its own.
	IgnoreIncoming,
	// The incoming instance wins: abandon ours and answer it.
	AnswerIncoming,
	// Exact id match: abandon ours and ignore the incoming one.
	AbortBoth,
};

[[nodiscard]] inline ConcurrentDecision DecideConcurrent(
		int64_t ours,
		int64_t incoming) {
	if (ours == incoming) {
		return ConcurrentDecision::AbortBoth;
	}
	return (ours > incoming)
		? ConcurrentDecision::IgnoreIncoming
		: ConcurrentDecision::AnswerIncoming;
}

// Rotation trigger predicate: replace the key once it has been used to
// encrypt and decrypt more than afterUses messages, or once it is older
// than afterTime provided it encrypted at least one message.
[[nodiscard]] inline bool ShouldRekey(
		int usesOut,
		int usesIn,
		int installedAt,
		int now,
		int afterUses,
		int afterTime) {
	const auto overused = (usesOut + usesIn) > afterUses;
	const auto aged = (usesOut > 0) && ((now - installedAt) > afterTime);
	return overused || aged;
}

} // namespace AyuSecret
