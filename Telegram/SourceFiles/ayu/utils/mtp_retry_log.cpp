#include "ayu/utils/mtp_retry_log.h"

#include "base/flat_map.h"
#include "logs.h"

#include <crl/crl_time.h>

#include <algorithm>
#include <vector>

namespace FreshMtp {
namespace {

constexpr auto kPeriod = crl::time(60 * 1000);
constexpr auto kTopKinds = 5;

struct State {
	base::flat_map<QString, int> retried;
	int dropped = 0;
	crl::time since = 0;
};

State &Current() {
	static auto result = State();
	return result;
}

void Flush(State &state, crl::time now) {
	if (state.since && now - state.since < kPeriod) {
		return;
	}
	if (state.since && (state.dropped || !state.retried.empty())) {
		auto rows = std::vector<std::pair<int, QString>>();
		auto total = 0;
		for (const auto &[key, count] : state.retried) {
			rows.emplace_back(count, key);
			total += count;
		}
		std::sort(rows.rbegin(), rows.rend());
		LOG(("MTP: %1 retried and %2 dropped before the resend in the last minute.")
			.arg(total)
			.arg(state.dropped));
		for (auto i = 0; i != std::min<int>(kTopKinds, rows.size()); ++i) {
			LOG(("MTP: %1 x %2").arg(rows[i].first).arg(rows[i].second));
		}
	}
	state.retried.clear();
	state.dropped = 0;
	state.since = now;
}

} // namespace

void NoteRetry(int code, const QString &type, uint32 body) {
	auto &state = Current();
	Flush(state, crl::now());
	const auto key = QString("code %1, %2, request 0x%3")
		.arg(code)
		.arg(type)
		.arg(QString::number(body, 16));
	++state.retried[key];
}

void NoteDropped() {
	auto &state = Current();
	Flush(state, crl::now());
	++state.dropped;
}

} // namespace FreshMtp
