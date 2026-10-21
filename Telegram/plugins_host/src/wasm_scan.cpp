#include "wasm_scan.h"

namespace fgpl {
namespace {

class Cursor {
public:
	Cursor(const uint8_t *data, size_t size)
	: _data(data)
	, _size(size) {
	}

	[[nodiscard]] bool done() const {
		return _position >= _size;
	}

	[[nodiscard]] size_t position() const {
		return _position;
	}

	[[nodiscard]] bool byte(uint8_t &out) {
		if (_position >= _size) {
			return false;
		}
		out = _data[_position++];
		return true;
	}

	[[nodiscard]] bool leb(uint64_t &out) {
		out = 0;
		for (auto shift = 0; shift != 70; shift += 7) {
			auto value = uint8_t(0);
			if (!byte(value)) {
				return false;
			}
			if (shift < 64) {
				out |= uint64_t(value & 0x7F) << shift;
			}
			if ((value & 0x80) == 0) {
				return true;
			}
		}
		return false;
	}

	[[nodiscard]] bool skip(uint64_t count) {
		if (count > _size - _position) {
			return false;
		}
		_position += size_t(count);
		return true;
	}

	[[nodiscard]] bool name(std::string &out) {
		auto length = uint64_t(0);
		if (!leb(length) || length > 1024 || length > _size - _position) {
			return false;
		}
		out.assign(
			reinterpret_cast<const char *>(_data + _position),
			size_t(length));
		_position += size_t(length);
		return true;
	}

private:
	const uint8_t *_data = nullptr;
	size_t _size = 0;
	size_t _position = 0;

};

[[nodiscard]] bool SkipLimits(Cursor &c) {
	auto flags = uint64_t(0);
	auto value = uint64_t(0);
	return c.leb(flags) && c.leb(value) && ((flags & 1) == 0 || c.leb(value));
}

} // namespace

Result<WasmInfo, std::string> ScanWasm(std::span<const uint8_t> module) {
	if (module.size() < 8
		|| module[0] != 0 || module[1] != 'a' || module[2] != 's' || module[3] != 'm'
		|| module[4] != 1 || module[5] != 0 || module[6] != 0 || module[7] != 0) {
		return std::string("not a WebAssembly module");
	}
	auto info = WasmInfo();
	auto cursor = Cursor(module.data() + 8, module.size() - 8);
	const auto bad = [] { return std::string("a malformed section"); };
	while (!cursor.done()) {
		auto id = uint8_t(0);
		auto size = uint64_t(0);
		if (!cursor.byte(id) || !cursor.leb(size)
			|| size > module.size() - 8 - cursor.position()) {
			return bad();
		}
		if (id != 2 && id != 7) {
			if (!cursor.skip(size)) {
				return bad();
			}
			continue;
		}
		auto section = Cursor(
			module.data() + 8 + cursor.position(),
			size_t(size));
		if (!cursor.skip(size)) {
			return bad();
		}
		auto count = uint64_t(0);
		if (!section.leb(count) || count > 100000) {
			return bad();
		}
		for (auto i = uint64_t(0); i != count; ++i) {
			auto first = std::string();
			auto second = std::string();
			auto kind = uint8_t(0);
			auto index = uint64_t(0);
			if (id == 7) {
				if (!section.name(second) || !section.byte(kind) || !section.leb(index)) {
					return bad();
				}
				info.exports.insert(second);
				continue;
			}
			if (!section.name(first) || !section.name(second) || !section.byte(kind)) {
				return bad();
			}
			auto ok = true;
			auto extra = uint8_t(0);
			switch (kind) {
			case 0: ok = section.leb(index); break;
			case 1: ok = section.byte(extra) && SkipLimits(section); break;
			case 2: ok = SkipLimits(section); break;
			case 3: ok = section.byte(extra) && section.byte(extra); break;
			case 4: ok = section.byte(extra) && section.leb(index); break;
			default: ok = false;
			}
			if (!ok) {
				return bad();
			}
			info.imports.emplace_back(std::move(first), std::move(second));
		}
	}
	return info;
}

} // namespace fgpl
