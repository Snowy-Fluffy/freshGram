#pragma once

#include "data/data_peer.h"
#include "data/data_peer_id.h"

namespace FreshSecret {

constexpr auto kPeerBase = (uint64(1) << 46);
constexpr auto kPeerSpan = (uint64(1) << 33);

[[nodiscard]] inline uint64 PeerBareForChat(int chatId) {
	return kPeerBase + uint64(uint32_t(chatId)) * 2 + 1;
}

[[nodiscard]] inline bool IsSecretBare(uint64 bare) {
	return bare >= kPeerBase
		&& bare < kPeerBase + kPeerSpan
		&& (bare & 1);
}

[[nodiscard]] inline bool IsSecretPeer(const PeerData *peer) {
	return peer
		&& peer->isUser()
		&& IsSecretBare(peerToUser(peer->id).bare);
}

[[nodiscard]] inline bool IsSecretPeerId(PeerId id) {
	return peerIsUser(id) && IsSecretBare(peerToUser(id).bare);
}

[[nodiscard]] inline int ChatIdOfPeer(const PeerData *peer) {
	return IsSecretPeer(peer)
		? int((peerToUser(peer->id).bare - kPeerBase) / 2)
		: 0;
}

} // namespace FreshSecret
