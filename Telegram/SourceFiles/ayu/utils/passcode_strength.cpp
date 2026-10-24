// This is the source code of freshGram.
#include "ayu/utils/passcode_strength.h"

#include "lang/lang_keys.h"
#include "lang_auto.h"

#include <cmath>

namespace FreshPasscode {
namespace {

constexpr auto kMaxBits = 256;

[[nodiscard]] int AlphabetSize(const QString &passcode) {
	auto digits = false;
	auto lower = false;
	auto upper = false;
	auto symbols = false;
	auto other = false;
	for (const auto ch : passcode) {
		const auto code = ch.unicode();
		if (code >= '0' && code <= '9') {
			digits = true;
		} else if (code >= 'a' && code <= 'z') {
			lower = true;
		} else if (code >= 'A' && code <= 'Z') {
			upper = true;
		} else if (code < 0x80) {
			symbols = true;
		} else {
			other = true;
		}
	}
	return (digits ? 10 : 0)
		+ (lower ? 26 : 0)
		+ (upper ? 26 : 0)
		+ (symbols ? 33 : 0)
		+ (other ? 64 : 0);
}

} // namespace

int EstimateBits(const QString &passcode) {
	const auto alphabet = AlphabetSize(passcode);
	if (alphabet < 2) {
		return 0;
	}
	// WHY: repeats and runs like "aaaa" or "1234" add almost nothing.
	auto length = 0.;
	auto previous = QChar();
	auto step = 0;
	for (const auto ch : passcode) {
		const auto delta = previous.isNull()
			? 0
			: (int(ch.unicode()) - int(previous.unicode()));
		if (!previous.isNull() && (delta == 0 || (delta == step
			&& std::abs(delta) == 1))) {
			length += 0.25;
		} else {
			length += 1.;
		}
		step = delta;
		previous = ch;
	}
	const auto bits = length * std::log2(double(alphabet));
	return std::min(int(std::lround(bits)), kMaxBits);
}

Strength StrengthForBits(int bits) {
	return (bits <= 0)
		? Strength::None
		: (bits < 28)
		? Strength::Weak
		: (bits < 45)
		? Strength::Medium
		: (bits < 70)
		? Strength::Strong
		: Strength::VeryStrong;
}

QString StrengthText(const QString &passcode) {
	const auto bits = EstimateBits(passcode);
	const auto name = [&] {
		switch (StrengthForBits(bits)) {
		case Strength::None: return QString();
		case Strength::Weak: return tr::fresh_PasscodeStrengthWeak(tr::now);
		case Strength::Medium: return tr::fresh_PasscodeStrengthMedium(tr::now);
		case Strength::Strong: return tr::fresh_PasscodeStrengthStrong(tr::now);
		case Strength::VeryStrong:
			return tr::fresh_PasscodeStrengthVeryStrong(tr::now);
		}
		return QString();
	}();
	return name.isEmpty()
		? QString()
		: tr::fresh_PasscodeStrengthBits(
			tr::now,
			lt_strength,
			name,
			lt_bits,
			QString::number(bits));
}

} // namespace FreshPasscode
