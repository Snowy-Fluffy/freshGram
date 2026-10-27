// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"
#include "crl/crl_time.h"

#include <string>

namespace fgpl {
class Transport;
} // namespace fgpl

namespace FreshPlugins {

struct ConnectionView {
	int64 id = 0;
	QString plugin;
	QString account;
	QString kind;
	QString host;
	int port = 0;
	QString address;
	QString via;
	bool tls = false;
	uint64 received = 0;
	uint64 sent = 0;
	crl::time opened = 0;
};

struct NetworkEvent {
	crl::time when = 0;
	QString plugin;
	QString kind;
	QString action;
	QString target;
	QString result;
	bool failed = false;
	uint64 received = 0;
	uint64 sent = 0;
	crl::time duration = 0;
};

struct NetworkProxy {
	enum class Type {
		None,
		Socks5,
		Http,
	};

	Type type = Type::None;
	std::string host;
	int port = 0;
	std::string user;
	std::string password;
};

struct NetworkPolicy {
	bool blockHttps = false;
	bool blockTcp = false;
	bool blockUdp = false;
	NetworkProxy proxy;
};

void SetNetworkPolicy(NetworkPolicy policy);

[[nodiscard]] fgpl::Transport *NetworkTransport();
[[nodiscard]] std::vector<NetworkEvent> NetworkLog();
[[nodiscard]] uint64 NetworkLogVersion();
void ClearNetworkLog();
[[nodiscard]] std::vector<ConnectionView> Connections();
void CloseConnection(int64 id);

} // namespace FreshPlugins
