#pragma once

#include "base/basic_types.h"
#include "data/data_msg_id.h"
#include "rpl/lifetime.h"

class History;
class HistoryItem;
class PeerData;
class ChannelData;
class ChatData;

namespace Data {
class Session;
class Forum;
class ForumTopic;
} // namespace Data

namespace FreshKept {

[[nodiscard]] bool keepDeletedTopic(not_null<Data::Forum*> forum, MsgId rootId);
[[nodiscard]] bool purgeDeletedTopic(not_null<Data::Forum*> forum, MsgId rootId);
[[nodiscard]] bool isKeptDeletedTopic(not_null<Data::ForumTopic*> topic);
void releaseDeletedTopics();

void setup(not_null<Data::Session*> owner, rpl::lifetime &lifetime);

void markLost(not_null<History*> history);
[[nodiscard]] bool keepOnDelete(not_null<History*> history);
void note(not_null<History*> history);
void forget(not_null<PeerData*> peer);
[[nodiscard]] bool showsDeleted(not_null<PeerData*> peer);
[[nodiscard]] bool isKept(not_null<PeerData*> peer);
[[nodiscard]] bool hasListableLastMessage(not_null<const History*> history);
[[nodiscard]] bool hasKeptTopics(not_null<const History*> history);
[[nodiscard]] bool serverUnavailable(not_null<const History*> history);
void revive(not_null<History*> history, not_null<HistoryItem*> item);
void checkWiped(not_null<History*> history);

void userLeaving(not_null<PeerData*> peer);
[[nodiscard]] bool takeUserLeaving(not_null<PeerData*> peer);
void chatAmInChanged(not_null<ChatData*> chat);

} // namespace FreshKept
