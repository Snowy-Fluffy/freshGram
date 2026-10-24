#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>

namespace FreshMtp {

void NoteRetry(int code, const QString &type, uint32 body);
void NoteDropped();

} // namespace FreshMtp
