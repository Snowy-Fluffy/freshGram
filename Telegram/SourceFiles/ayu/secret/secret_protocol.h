#pragma once

#include "ayu/secret/secret_model.h"

#include <string>

namespace AyuSecret {

enum class ActionKind {
	None,
	NotifyLayer,
	Resend,
	ReadMessages,
	DeleteMessages,
	ScreenshotMessages,
	FlushHistory,
	SetTtl,
	Typing,
	RequestKey,
	AcceptKey,
	AbortKey,
	CommitKey,
	Noop,
	Other,
};

struct Inbound {
	int layer = 0;
	int inSeqNo = 0;
	int outSeqNo = 0;
	bool service = false;
	int64_t randomId = 0;
	std::string text;
	std::vector<Entity> entities;
	int64_t replyTo = 0;
	int ttl = 0;
	Media media;
	ActionKind action = ActionKind::None;
	int actionValue = 0;
	int resendStart = 0;
	int resendEnd = 0;
	std::vector<int64_t> ids;
	int64_t exchangeId = 0;
	Bytes value;
	int64_t fingerprint = 0;
};

[[nodiscard]] bool ParseLayerObject(const Bytes &object, Inbound &result);

[[nodiscard]] Bytes BuildLayerObject(
	const Bytes &message,
	int inSeqNo,
	int outSeqNo);

[[nodiscard]] Bytes BuildMessage(const MessageData &data);

[[nodiscard]] Bytes BuildNotifyLayer(int64_t randomId, int layer);
[[nodiscard]] Bytes BuildResend(int64_t randomId, int start, int end);
[[nodiscard]] Bytes BuildNoop(int64_t randomId);
[[nodiscard]] Bytes BuildFlushHistory(int64_t randomId);
[[nodiscard]] Bytes BuildSetTtl(int64_t randomId, int ttl);
[[nodiscard]] Bytes BuildReadMessages(
	int64_t randomId,
	const std::vector<int64_t> &ids);
[[nodiscard]] Bytes BuildDeleteMessages(
	int64_t randomId,
	const std::vector<int64_t> &ids);
[[nodiscard]] Bytes BuildScreenshot(
	int64_t randomId,
	const std::vector<int64_t> &ids);
[[nodiscard]] Bytes BuildRequestKey(
	int64_t randomId,
	int64_t exchangeId,
	const Bytes &gA);
[[nodiscard]] Bytes BuildAcceptKey(
	int64_t randomId,
	int64_t exchangeId,
	const Bytes &gB,
	int64_t fingerprint);
[[nodiscard]] Bytes BuildCommitKey(
	int64_t randomId,
	int64_t exchangeId,
	int64_t fingerprint);
[[nodiscard]] Bytes BuildAbortKey(int64_t randomId, int64_t exchangeId);

} // namespace AyuSecret
