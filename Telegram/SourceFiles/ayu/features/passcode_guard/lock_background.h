// This is the source code of freshGram.
#pragma once

namespace FreshLockBackground {

// Reads the chosen file, checks it and keeps it re-encoded as JPEG.
void Import(const QString &path, Fn<void(bool)> done);
void Remove();

// The picture instead of the lock icon, kept as a square JPEG.
void ImportAvatar(const QString &path, Fn<void(bool)> done);
void RemoveAvatar();
void ClearAvatar();
// The square in device pixels, null while it is not ready.
[[nodiscard]] QImage PreparedAvatar(int side);

// The image fitted to the size in device pixels, null while it is not ready.
[[nodiscard]] QImage Prepared(QSize size, bool blur, int dim);
[[nodiscard]] rpl::producer<> Updates();
void Clear();

} // namespace FreshLockBackground
