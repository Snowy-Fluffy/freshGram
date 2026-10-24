#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace FreshSecret {

constexpr auto kLayer = 73;

constexpr uint32_t kDecryptedMessageLayer = 0x1be31789U;
constexpr uint32_t kDecryptedMessage = 0x91cc4674U;
constexpr uint32_t kDecryptedMessage46 = 0x36b091deU;
constexpr uint32_t kDecryptedMessage23 = 0x204d3878U;
constexpr uint32_t kDecryptedMessageService = 0x73164160U;
constexpr uint32_t kActionSetMessageTTL = 0xa1733aecU;
constexpr uint32_t kActionReadMessages = 0x0c4f40beU;
constexpr uint32_t kActionDeleteMessages = 0x65614304U;
constexpr uint32_t kActionScreenshotMessages = 0x8ac1f475U;
constexpr uint32_t kActionFlushHistory = 0x6719e45cU;
constexpr uint32_t kActionResend = 0x511110b0U;
constexpr uint32_t kActionNotifyLayer = 0xf3048883U;
constexpr uint32_t kActionTyping = 0xccb27641U;
constexpr uint32_t kActionRequestKey = 0xf3c9611bU;
constexpr uint32_t kActionAcceptKey = 0x6fe1735bU;
constexpr uint32_t kActionAbortKey = 0xdd05ec6bU;
constexpr uint32_t kActionCommitKey = 0xec2e0b9bU;
constexpr uint32_t kActionNoop = 0xa82fdd63U;

class Writer {
public:
	void writeInt(int32_t value) {
		write(&value, sizeof(value));
	}
	void writeUInt(uint32_t value) {
		write(&value, sizeof(value));
	}
	void writeLong(int64_t value) {
		write(&value, sizeof(value));
	}
	void writeDouble(double value) {
		write(&value, sizeof(value));
	}
	void writeBytes(const uint8_t *data, size_t size) {
		if (size < 254) {
			_data.push_back(uint8_t(size));
			_data.insert(_data.end(), data, data + size);
			pad(1 + size);
		} else {
			_data.push_back(254);
			_data.push_back(uint8_t(size & 0xFF));
			_data.push_back(uint8_t((size >> 8) & 0xFF));
			_data.push_back(uint8_t((size >> 16) & 0xFF));
			_data.insert(_data.end(), data, data + size);
			pad(4 + size);
		}
	}
	void writeString(const std::string &value) {
		writeBytes(
			reinterpret_cast<const uint8_t*>(value.data()),
			value.size());
	}
	void writeRaw(const std::vector<uint8_t> &value) {
		_data.insert(_data.end(), value.begin(), value.end());
	}
	[[nodiscard]] const std::vector<uint8_t> &data() const {
		return _data;
	}

private:
	void write(const void *data, size_t size) {
		const auto bytes = static_cast<const uint8_t*>(data);
		_data.insert(_data.end(), bytes, bytes + size);
	}
	void pad(size_t written) {
		while (written % 4) {
			_data.push_back(0);
			++written;
		}
	}

	std::vector<uint8_t> _data;

};

class Reader {
public:
	Reader(const uint8_t *data, size_t size)
	: _data(data)
	, _size(size) {
	}

	[[nodiscard]] bool failed() const {
		return _failed;
	}
	[[nodiscard]] size_t left() const {
		return _size - _position;
	}
	int32_t readInt() {
		int32_t result = 0;
		read(&result, sizeof(result));
		return result;
	}
	uint32_t readUInt() {
		uint32_t result = 0;
		read(&result, sizeof(result));
		return result;
	}
	int64_t readLong() {
		int64_t result = 0;
		read(&result, sizeof(result));
		return result;
	}
	double readDouble() {
		double result = 0;
		read(&result, sizeof(result));
		return result;
	}
	std::vector<uint8_t> readBytes() {
		auto result = std::vector<uint8_t>();
		if (left() < 1) {
			_failed = true;
			return result;
		}
		auto length = size_t(_data[_position++]);
		auto header = size_t(1);
		if (length == 254) {
			if (left() < 3) {
				_failed = true;
				return result;
			}
			length = size_t(_data[_position])
				| (size_t(_data[_position + 1]) << 8)
				| (size_t(_data[_position + 2]) << 16);
			_position += 3;
			header = 4;
		} else if (length == 255) {
			_failed = true;
			return result;
		}
		if (left() < length) {
			_failed = true;
			return result;
		}
		result.assign(_data + _position, _data + _position + length);
		_position += length;
		const auto total = header + length;
		const auto padding = (4 - (total % 4)) % 4;
		if (left() < padding) {
			_failed = true;
			return result;
		}
		_position += padding;
		return result;
	}
	std::string readString() {
		const auto bytes = readBytes();
		return std::string(bytes.begin(), bytes.end());
	}

private:
	void read(void *to, size_t size) {
		if (left() < size) {
			_failed = true;
			return;
		}
		std::memcpy(to, _data + _position, size);
		_position += size;
	}

	const uint8_t *_data = nullptr;
	size_t _size = 0;
	size_t _position = 0;
	bool _failed = false;

};

} // namespace FreshSecret
