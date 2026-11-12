#pragma once

namespace Data {
class Thread;
} // namespace Data

namespace Window {
class SessionController;
} // namespace Window

namespace AyuFeatures::MessageShot::Sticker {

// WEBP with the longest side of 512px, empty if it doesn't fit in 512KB.
[[nodiscard]] QByteArray Encode(QImage shot);

void Send(
	not_null<Window::SessionController*> controller,
	not_null<Data::Thread*> thread,
	const QByteArray &webp);

// |done| gets true if the sticker was added to a pack.
void AddToPack(
	not_null<Window::SessionController*> controller,
	const QByteArray &webp,
	const QString &emoji,
	Fn<void(bool)> done);

} // namespace AyuFeatures::MessageShot::Sticker
