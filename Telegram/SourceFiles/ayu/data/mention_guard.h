#pragma once

namespace Data {
class Thread;
} // namespace Data

namespace FreshMentions {

void GuardJump(not_null<Data::Thread*> thread, MsgId msgId);

} // namespace FreshMentions
