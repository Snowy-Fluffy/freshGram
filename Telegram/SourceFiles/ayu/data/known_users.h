#pragma once

#include "base/basic_types.h"

class UserData;

namespace Main {
class Session;
} // namespace Main

namespace FreshUsers {

void note(not_null<UserData*> user);

[[nodiscard]] UserData *find(not_null<Main::Session*> session, UserId id);

} // namespace FreshUsers
