// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/utils/network_privacy.h"

#include <algorithm>
#include <atomic>
#include <iterator>

namespace FreshNetworkPrivacy {
namespace {

constexpr int kLadder[] = { 3600, 6 * 3600, 86400 };
constexpr auto kLadderTop = int(std::size(kLadder)) - 1;

std::atomic<bool> Enabled = false;
std::atomic<int> Step = 0;

} // namespace

void SetEnabled(bool enabled) {
	if (enabled && !Enabled.exchange(true)) {
		Step = 0;
	} else if (!enabled) {
		Enabled = false;
	}
}

int TemporaryKeyExpiresIn() {
	return Enabled ? kLadder[std::min(Step.load(), kLadderTop)] : kLadder[kLadderTop];
}

bool StepUpTemporaryKeyExpiresIn() {
	if (!Enabled || Step.load() >= kLadderTop) {
		return false;
	}
	++Step;
	return true;
}

} // namespace FreshNetworkPrivacy
