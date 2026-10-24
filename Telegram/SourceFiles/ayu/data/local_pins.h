// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "base/basic_types.h"

class History;
struct PeerId;

namespace Data {
class Folder;
class Session;
} // namespace Data

namespace Dialogs {
class Key;
} // namespace Dialogs

namespace FreshLocalPins {

constexpr auto kListLimit = 100000;

[[nodiscard]] bool isLocal(Dialogs::Key key);
[[nodiscard]] int count(
	not_null<const Data::Session*> owner,
	Data::Folder *folder);

void pin(not_null<History*> history);
[[nodiscard]] bool unpin(not_null<History*> history);
void forget(not_null<Data::Session*> owner, PeerId peerId);
void reapply(not_null<Data::Session*> owner);
void saveOrder(not_null<Data::Session*> owner);

} // namespace FreshLocalPins
