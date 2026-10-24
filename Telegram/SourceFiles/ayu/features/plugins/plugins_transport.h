// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"
#include "crl/crl_time.h"

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
	bool tls = false;
	uint64 received = 0;
	uint64 sent = 0;
	crl::time opened = 0;
};

[[nodiscard]] fgpl::Transport *NetworkTransport();
[[nodiscard]] std::vector<ConnectionView> Connections();
void CloseConnection(int64 id);

} // namespace FreshPlugins
