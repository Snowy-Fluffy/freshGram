#pragma once

#include "api/api_common.h"
#include "storage/localimageloader.h"

namespace Ui {
struct PreparedList;
} // namespace Ui

namespace Data {
class Thread;
} // namespace Data

namespace FreshSecret {

[[nodiscard]] bool SendText(const Api::MessageToSend &message);
[[nodiscard]] bool SendFiles(
	Ui::PreparedList &list,
	const Api::SendAction &action);
[[nodiscard]] bool SendBytes(
	const QByteArray &bytes,
	const Api::SendAction &action);
[[nodiscard]] bool SendVoice(
	const QByteArray &bytes,
	const VoiceWaveform &waveform,
	crl::time duration,
	bool video,
	const Api::SendAction &action);
[[nodiscard]] bool SendDocument(
	const Api::MessageToSend &message,
	not_null<DocumentData*> document);
[[nodiscard]] bool Reject(not_null<PeerData*> peer);

[[nodiscard]] bool Forward(
	const std::vector<not_null<HistoryItem*>> &items,
	std::vector<not_null<Data::Thread*>> &threads,
	const Api::SendOptions &options);

} // namespace FreshSecret
