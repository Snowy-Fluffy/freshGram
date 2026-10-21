#include "fgpl/ipc.h"
#include "fgpl/protocol.h"

#include <signal.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace fgpl;

int gFd = -1;

void Raw(const void *data, size_t size) {
	auto bytes = static_cast<const uint8_t *>(data);
	while (size != 0) {
		const auto written = write(gFd, bytes, size);
		if (written <= 0) {
			std::_Exit(0);
		}
		bytes += written;
		size -= size_t(written);
	}
}

void Header(uint32_t size) {
	const uint8_t header[4] = {
		uint8_t(size >> 24),
		uint8_t(size >> 16),
		uint8_t(size >> 8),
		uint8_t(size),
	};
	Raw(header, 4);
}

void Body(const cbor::Bytes &body) {
	Header(uint32_t(body.size()));
	Raw(body.data(), body.size());
}

void Send(const cbor::Value &message) {
	auto body = cbor::Bytes();
	if (!cbor::Encode(message, body, 24)) {
		std::_Exit(5);
	}
	Body(body);
}

void Pause(int milliseconds) {
	std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

void Forever() {
	while (true) {
		Pause(1000);
	}
}

cbor::Value Hello() {
	return proto::Map({
		{ "t", proto::Text("hello") },
		{ "landlock_abi", proto::Int(0) },
		{ "landlock", proto::Bool(false) },
		{ "seccomp", proto::Bool(false) },
		{ "rlimits", proto::Bool(false) },
	});
}

cbor::Value Type(const char *type) {
	return proto::Map({ { "t", proto::Text(type) } });
}

cbor::Value Read() {
	auto frame = ipc::ReadFrame(gFd, ipc::TrustedLimits());
	if (!frame.ok()) {
		std::_Exit(0);
	}
	return std::move(frame.value());
}

void Garbage() {
	auto body = cbor::Bytes(64, 0xFF);
	Body(body);
}

void Deep() {
	auto body = cbor::Bytes(5000, 0x81);
	body.push_back(0);
	Body(body);
}

void BigCount() {
	auto body = cbor::Bytes{ 0x9A, 0x00, 0x01, 0x86, 0xA0 };
	body.insert(body.end(), 100000, 0);
	Body(body);
}

void ManyItems() {
	auto body = cbor::Bytes();
	body.push_back(0x8A);
	for (auto i = 0; i != 10; ++i) {
		body.insert(body.end(), { 0x99, 0x27, 0x10 });
		body.insert(body.end(), 10000, 0);
	}
	Body(body);
}

void HugeString() {
	Body({ 0x7A, 0x06, 0x40, 0x00, 0x00, 'a', 'b' });
}

void BadUtf8() {
	Body({ 0xA1, 0x61, 't', 0x62, 0xC3, 0x28 });
}

void DuplicateKeys() {
	Body({ 0xA2, 0x61, 't', 0x01, 0x61, 't', 0x02 });
}

void Slowloris(const cbor::Value &message) {
	auto body = cbor::Bytes();
	if (!cbor::Encode(message, body, 24)) {
		std::_Exit(5);
	}
	const auto size = uint32_t(body.size());
	const uint8_t header[4] = {
		uint8_t(size >> 24),
		uint8_t(size >> 16),
		uint8_t(size >> 8),
		uint8_t(size),
	};
	for (const auto byte : header) {
		Raw(&byte, 1);
		Pause(200);
	}
	for (const auto byte : body) {
		Raw(&byte, 1);
		Pause(200);
	}
}

void Crash() {
	std::signal(SIGSEGV, SIG_DFL);
	std::raise(SIGSEGV);
}

} // namespace

int main(int argc, char **argv) {
	auto attack = std::string();
	for (auto i = 1; i < argc; ++i) {
		const auto argument = std::string(argv[i]);
		if (argument.rfind("--ipc-fd=", 0) == 0) {
			gFd = std::atoi(argument.c_str() + 9);
		} else if (argument.rfind("--attack=", 0) == 0) {
			attack = argument.substr(9);
		}
	}
	if (gFd < 3) {
		return 64;
	}
	signal(SIGPIPE, SIG_IGN);

	if (attack == "no_hello") {
		Forever();
	} else if (attack == "hello_garbage") {
		Garbage();
		Forever();
	} else if (attack == "hello_wrong_type") {
		Send(Type("event"));
		Forever();
	} else if (attack == "hello_giant_length") {
		Header(0xFFFFFFFFu);
		Forever();
	} else if (attack == "hello_stall") {
		Header(100);
		Raw("0123456789", 10);
		Forever();
	} else if (attack == "hello_crash") {
		Crash();
	}
	Send(Hello());
	if (attack == "hello_then_exit") {
		return 0;
	}

	Read();
	if (attack == "load_giant_length") {
		Header(0xFFFFFFFFu);
		Forever();
	} else if (attack == "load_over_limit") {
		Header(uint32_t(ipc::kMaxFrameBytes) + 1);
		Forever();
	} else if (attack == "load_garbage") {
		Garbage();
		Forever();
	} else if (attack == "load_deep") {
		Deep();
		Forever();
	} else if (attack == "load_big_count") {
		BigCount();
		Forever();
	} else if (attack == "load_many_items") {
		ManyItems();
		Forever();
	} else if (attack == "load_huge_string") {
		HugeString();
		Forever();
	} else if (attack == "load_bad_utf8") {
		BadUtf8();
		Forever();
	} else if (attack == "load_duplicate_keys") {
		DuplicateKeys();
		Forever();
	} else if (attack == "load_wrong_reply") {
		Send(proto::Map({
			{ "t", proto::Text("done") },
			{ "id", proto::Int(1) },
			{ "status", proto::Int(0) },
		}));
		Forever();
	} else if (attack == "load_crash") {
		Crash();
	} else if (attack == "load_exit") {
		return 7;
	} else if (attack == "load_abort") {
		std::signal(SIGABRT, SIG_DFL);
		std::abort();
	}
	Send(Type("loaded"));

	const auto request = Read();
	const auto id = proto::IntField(request, "id");
	const auto done = [&] {
		return proto::Map({
			{ "t", proto::Text("done") },
			{ "id", proto::Int(id) },
			{ "status", proto::Int(0) },
		});
	};
	if (attack == "event_log_flood") {
		const auto line = std::string(100, 'x');
		for (auto i = 0; i != 60000; ++i) {
			Send(proto::Map({
				{ "t", proto::Text("log") },
				{ "line", proto::Text(line) },
			}));
		}
		Send(done());
	} else if (attack == "event_log_forever") {
		const auto line = std::string(1000, 'x');
		while (true) {
			Send(proto::Map({
				{ "t", proto::Text("log") },
				{ "line", proto::Text(line) },
			}));
		}
	} else if (attack == "event_call_flood") {
		for (auto i = 0; i != 100000; ++i) {
			Send(proto::Map({
				{ "t", proto::Text("call") },
				{ "v", proto::Int(i) },
			}));
		}
		Forever();
	} else if (attack == "event_call_unread_replies") {
		for (auto i = 0; i != 400; ++i) {
			Send(proto::Map({
				{ "t", proto::Text("call") },
				{ "v", proto::Int(i) },
			}));
		}
		Forever();
	} else if (attack == "event_call_huge_request") {
		Send(proto::Map({
			{ "t", proto::Text("call") },
			{ "v", proto::Text(std::string(size_t(3) << 20, 'a')) },
		}));
		Forever();
	} else if (attack == "event_wrong_id") {
		Send(proto::Map({
			{ "t", proto::Text("done") },
			{ "id", proto::Int(id + 1000) },
			{ "status", proto::Int(0) },
		}));
		Forever();
	} else if (attack == "event_unsolicited") {
		Send(Type("load_failed"));
		Forever();
	} else if (attack == "event_huge_detail") {
		Send(proto::Map({
			{ "t", proto::Text("failed") },
			{ "id", proto::Int(id) },
			{ "error", proto::Int(2) },
			{ "detail", proto::Text(std::string(size_t(3) << 20, 'd')) },
		}));
		Forever();
	} else if (attack == "event_slowloris") {
		Slowloris(proto::Map({
			{ "t", proto::Text("done") },
			{ "id", proto::Int(id) },
			{ "status", proto::Int(0) },
			{ "pad", proto::Text(std::string(80, 'p')) },
		}));
	} else if (attack == "event_crash") {
		Send(proto::Map({
			{ "t", proto::Text("call") },
			{ "v", proto::Int(1) },
		}));
		Crash();
	} else if (attack == "event_exit") {
		return 0;
	} else if (attack == "event_stuck") {
		Forever();
	} else if (attack == "event_garbage") {
		Garbage();
		Forever();
	} else {
		Send(done());
	}
	Forever();
}
