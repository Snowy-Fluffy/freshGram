// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_transport.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "core/core_settings_proxy.h"
#include "mtproto/mtproto_proxy_data.h"

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/broker.h"
#include "fgpl/net_policy.h"
#endif

#include <QtCore/QDeadlineTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QHostInfo>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslSocket>

#include <mutex>

namespace AyuPlugins {

#ifdef FRESHGRAM_PLUGINS

namespace {

constexpr auto kMaxRedirects = 5;
constexpr auto kMaxHeaderBytes = size_t(64) << 10;

struct ProxyChoice {
	enum class Mode {
		Direct,
		Application,
		Proxy,
		Refuse,
	};

	Mode mode = Mode::Direct;
	QNetworkProxy proxy;
};

std::mutex &ProxyMutex() {
	static auto result = std::mutex();
	return result;
}

ProxyChoice &Choice() {
	static auto result = ProxyChoice();
	return result;
}

[[nodiscard]] ProxyChoice CurrentChoice() {
	const auto lock = std::scoped_lock(ProxyMutex());
	return Choice();
}

[[nodiscard]] fgpl::FetchError Error(const char *code, QString message) {
	return { code, message.toStdString() };
}

[[nodiscard]] std::string Lower(std::string text) {
	for (auto &ch : text) {
		if (ch >= 'A' && ch <= 'Z') {
			ch = char(ch - 'A' + 'a');
		}
	}
	return text;
}

struct Parsed {
	int status = 0;
	std::vector<std::pair<std::string, std::string>> headers;
	std::string location;
	bool chunked = false;
	std::optional<size_t> length;
	size_t consumed = 0;
};

[[nodiscard]] std::optional<Parsed> ParseHead(const std::string &data) {
	const auto end = data.find("\r\n\r\n");
	if (end == std::string::npos) {
		return std::nullopt;
	}
	auto result = Parsed();
	result.consumed = end + 4;
	auto line = size_t(0);
	auto first = true;
	while (line < end + 2) {
		const auto next = data.find("\r\n", line);
		const auto text = data.substr(line, next - line);
		line = next + 2;
		if (first) {
			first = false;
			if (text.rfind("HTTP/1.", 0) != 0 || text.size() < 12) {
				return Parsed{ .status = -1 };
			}
			result.status = std::atoi(text.c_str() + 9);
			continue;
		}
		const auto colon = text.find(':');
		if (colon == std::string::npos || colon == 0) {
			continue;
		}
		auto name = Lower(text.substr(0, colon));
		auto value = text.substr(colon + 1);
		while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
			value.erase(value.begin());
		}
		while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
			value.pop_back();
		}
		if (name == "set-cookie" || name == "set-cookie2") {
			continue;
		} else if (name == "location") {
			result.location = value;
		} else if (name == "transfer-encoding") {
			result.chunked = (Lower(value).find("chunked") != std::string::npos);
		} else if (name == "content-length") {
			char *tail = nullptr;
			const auto parsed = std::strtoull(value.c_str(), &tail, 10);
			if (tail == value.c_str() || *tail != 0) {
				return Parsed{ .status = -1 };
			}
			result.length = size_t(parsed);
		}
		result.headers.emplace_back(std::move(name), std::move(value));
	}
	return result;
}

[[nodiscard]] bool DecodeChunked(
		const std::string &raw,
		size_t maxBytes,
		std::string &out,
		bool &complete) {
	auto at = size_t(0);
	complete = false;
	out.clear();
	while (true) {
		const auto eol = raw.find("\r\n", at);
		if (eol == std::string::npos) {
			return true;
		}
		char *tail = nullptr;
		const auto size = std::strtoull(raw.c_str() + at, &tail, 16);
		if (tail == raw.c_str() + at) {
			return false;
		}
		at = eol + 2;
		if (size == 0) {
			complete = true;
			return true;
		}
		if (out.size() + size > maxBytes) {
			return false;
		}
		if (raw.size() < at + size + 2) {
			return true;
		}
		out.append(raw, at, size);
		at += size + 2;
	}
}

class NetTransport final : public fgpl::Transport {
public:
	fgpl::Result<fgpl::FetchResponse, fgpl::FetchError> fetch(
			const fgpl::FetchRequest &request) override {
		auto url = request.url;
		auto method = request.method;
		auto body = request.body;
		auto headers = request.headers;
		auto host = request.host;
		auto previousHost = host;
		const auto deadline = QDeadlineTimer(request.timeoutMs);
		for (auto hop = 0; hop <= kMaxRedirects; ++hop) {
			const auto parsed = fgpl::net::ParseUrl(url);
			if (!parsed.ok()) {
				return Error("invalid", u"url"_q);
			}
			auto response = once(
				parsed.value(),
				method,
				headers,
				body,
				request.maxResponseBytes,
				deadline);
			if (!response.ok()) {
				return response.error();
			}
			const auto &value = response.value();
			const auto redirect = (value.first.status == 301
				|| value.first.status == 302
				|| value.first.status == 303
				|| value.first.status == 307
				|| value.first.status == 308);
			if (!redirect || value.second.empty()) {
				return std::move(response.value().first);
			}
			const auto next = QUrl(QString::fromStdString(url))
				.resolved(QUrl(QString::fromStdString(value.second)));
			const auto target = next.toString(QUrl::FullyEncoded).toStdString();
			if (next.scheme() != u"https"_q
				|| !request.allowRedirect
				|| !request.allowRedirect(target)) {
				return Error("redirect", u"a redirect is not allowed"_q);
			}
			const auto nextParsed = fgpl::net::ParseUrl(target);
			if (!nextParsed.ok()) {
				return Error("redirect", u"a redirect is not allowed"_q);
			}
			if (nextParsed.value().host != parsed.value().host) {
				std::erase_if(headers, [](const auto &entry) {
					return Lower(entry.first) == "authorization";
				});
			}
			if (value.first.status == 303
				|| ((value.first.status == 301 || value.first.status == 302)
					&& method == "POST")) {
				method = "GET";
				body.clear();
			}
			url = nextParsed.value().text;
		}
		return Error("redirect", u"too many redirects"_q);
	}

private:
	using Single = fgpl::Result<
		std::pair<fgpl::FetchResponse, std::string>,
		fgpl::FetchError>;

	[[nodiscard]] Single once(
			const fgpl::net::Url &url,
			const std::string &method,
			const std::vector<std::pair<std::string, std::string>> &headers,
			const fgpl::cbor::Bytes &body,
			size_t maxBytes,
			const QDeadlineTimer &deadline) {
		const auto choice = CurrentChoice();
		if (choice.mode == ProxyChoice::Mode::Refuse) {
			return Error("proxy", u"the selected proxy cannot carry plugin traffic"_q);
		}
		const auto host = QString::fromStdString(url.host);
		const auto lookup = QHostInfo::fromName(host);
		if (lookup.error() != QHostInfo::NoError || lookup.addresses().isEmpty()) {
			return Error("dns", u"the host was not found"_q);
		}
		for (const auto &address : lookup.addresses()) {
			if (!fgpl::net::IsPublicAddress(address.toString().toStdString())) {
				return Error("blocked", u"the address is not public"_q);
			}
		}
		auto socket = QSslSocket();
		auto configuration = QSslConfiguration::defaultConfiguration();
		configuration.setProtocol(QSsl::TlsV1_2OrLater);
		socket.setSslConfiguration(configuration);
		socket.setPeerVerifyMode(QSslSocket::VerifyPeer);
		socket.setPeerVerifyName(host);
		if (choice.mode == ProxyChoice::Mode::Proxy) {
			socket.setProxy(choice.proxy);
			socket.connectToHost(host, quint16(url.port));
		} else {
			socket.setProxy(choice.mode == ProxyChoice::Mode::Application
				? QNetworkProxy(QNetworkProxy::DefaultProxy)
				: QNetworkProxy(QNetworkProxy::NoProxy));
			socket.connectToHost(lookup.addresses().front(), quint16(url.port));
		}
		if (!socket.waitForConnected(remaining(deadline))) {
			return Error("connect", socket.errorString());
		}
		socket.startClientEncryption();
		if (!socket.waitForEncrypted(remaining(deadline))) {
			return Error("tls", socket.errorString());
		}

		auto head = method + " " + url.target + " HTTP/1.1\r\nHost: " + url.host;
		if (url.port != 443) {
			head += ":" + std::to_string(url.port);
		}
		head += "\r\nConnection: close\r\nAccept-Encoding: identity\r\n";
		auto hasAgent = false;
		for (const auto &[name, value] : headers) {
			hasAgent = hasAgent || (Lower(name) == "user-agent");
			head += name + ": " + value + "\r\n";
		}
		if (!hasAgent) {
			head += "User-Agent: freshGram-plugin\r\n";
		}
		if (!body.empty() || method == "POST" || method == "PUT" || method == "PATCH") {
			head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
		}
		head += "\r\n";
		socket.write(head.data(), qint64(head.size()));
		if (!body.empty()) {
			socket.write(reinterpret_cast<const char*>(body.data()), qint64(body.size()));
		}
		if (!socket.waitForBytesWritten(remaining(deadline))) {
			return Error("write", socket.errorString());
		}

		auto raw = std::string();
		auto parsed = std::optional<Parsed>();
		auto payload = std::string();
		auto done = false;
		while (!done) {
			if (!socket.bytesAvailable()
				&& socket.state() == QAbstractSocket::ConnectedState
				&& !socket.waitForReadyRead(remaining(deadline))
				&& socket.state() == QAbstractSocket::ConnectedState) {
				return Error("timeout", u"the request timed out"_q);
			}
			const auto chunk = socket.readAll();
			raw.append(chunk.constData(), size_t(chunk.size()));
			const auto closed = (socket.state() != QAbstractSocket::ConnectedState
				&& !socket.bytesAvailable());
			if (!parsed) {
				if (raw.size() > kMaxHeaderBytes && raw.find("\r\n\r\n") == std::string::npos) {
					return Error("response", u"the headers are too large"_q);
				}
				parsed = ParseHead(raw);
				if (parsed && parsed->status < 0) {
					return Error("response", u"a malformed response"_q);
				}
			}
			if (parsed) {
				const auto available = raw.size() - parsed->consumed;
				if (parsed->chunked) {
					auto complete = false;
					if (!DecodeChunked(raw.substr(parsed->consumed), maxBytes, payload, complete)) {
						return Error("response", u"a malformed or too large body"_q);
					}
					done = complete;
				} else if (parsed->length) {
					if (*parsed->length > maxBytes) {
						return Error("limit", u"the response is too large"_q);
					}
					done = (available >= *parsed->length);
					if (done) {
						payload = raw.substr(parsed->consumed, *parsed->length);
					}
				} else {
					if (available > maxBytes) {
						return Error("limit", u"the response is too large"_q);
					}
					if (closed) {
						payload = raw.substr(parsed->consumed);
						done = true;
					}
				}
			}
			if (!done && closed) {
				return Error("response", u"the connection closed early"_q);
			}
		}
		socket.abort();

		auto response = fgpl::FetchResponse();
		response.status = parsed->status;
		response.headers = parsed->headers;
		response.body.assign(payload.begin(), payload.end());
		return std::make_pair(std::move(response), parsed->location);
	}

	[[nodiscard]] static int remaining(const QDeadlineTimer &deadline) {
		return int(std::max<qint64>(1, deadline.remainingTime()));
	}

};

} // namespace

fgpl::Transport *NetworkTransport() {
	static auto result = NetTransport();
	return &result;
}

void RefreshProxy() {
	auto choice = ProxyChoice();
	const auto &settings = Core::App().settings().proxy();
	if (settings.isSystem()) {
		choice.mode = ProxyChoice::Mode::Application;
	} else if (settings.isEnabled()) {
		const auto selected = settings.selected();
		if (selected.type == MTP::ProxyData::Type::Socks5
			|| selected.type == MTP::ProxyData::Type::Http) {
			choice.mode = ProxyChoice::Mode::Proxy;
			choice.proxy = MTP::ToNetworkProxy(selected);
		} else {
			choice.mode = ProxyChoice::Mode::Refuse;
		}
	}
	const auto lock = std::scoped_lock(ProxyMutex());
	Choice() = std::move(choice);
}

#else

fgpl::Transport *NetworkTransport() {
	return nullptr;
}

void RefreshProxy() {
}

#endif

} // namespace AyuPlugins
