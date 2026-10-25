# freshGram

[ English | [Русский](README-RU.md) ]

freshGram is a [Telegram Desktop](https://github.com/telegramdesktop/tdesktop) fork that combines
the Material Design look and customization of [materialgram](https://github.com/kukuruzka165/materialgram)
with all the features of [AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop).
It is based on the [Telegram API][telegram_api] and the [MTProto][telegram_proto] secure protocol.

The source code is published under GPLv3 with OpenSSL exception, the license is available [here][license].

## Features

### From AyuGram Desktop

- Full ghost mode (flexible)
- Messages history: the edits of other people and your own and the deleted messages are kept in a local database, which is backed up automatically (the newest valid backup is restored if the database gets corrupted)
- Storage settings (Settings - freshGram - Storage): how many edits are kept per message, how many deleted messages are loaded when a chat opens, how many messages are saved when you leave a chat, the size limit of saved deleted media, how long deleted messages and edits are kept, and how many database backups are made and how often
- Anti-recall
- Message type filter in the search inside a chat (photos, videos, files, music, voice, video messages, links, GIFs), also together with the "From" user
- "Hide birthday notifications" setting: hides the birthday banners over the chats list, the profile confetti, the cake and the forced gift button
- "Peek last seen" in a user profile (without Telegram Premium): briefly adds the user to the "Last seen & online" exceptions, reads their exact status and restores your privacy settings; the result is shown as a separate profile line and next to the status in the chat top bar and the profile header, for example "last seen recently (peeked: last seen yesterday at 11:27 PM)"
- Secret chats live in the main chat list (opt-in: Settings - freshGram - Enable secret chats, requires a local passcode): text with formatting, replies, photos, files, static stickers and GIFs, voice messages and video notes recorded with the native composer, self-destruct timers, typing and read marks, forwarding, message shots, native media viewer, encryption key picture and hex for comparison. Ended chats stay in the list, real notifications show only "Secret chat" and "New secret message", requests that arrived while freshGram was closed are picked up after a restart. Keys, messages and attachments are encrypted with a key derived from the local passcode key, together with chat metadata and bound to their chat and message; removing the passcode or logging out deletes secret chats, and database backups keep secret data only in the newest snapshots, links are never previewed through Telegram servers
- Service messages (for example, "user joined") are kept when deleted, the same way as normal ones
- Deleted topics of groups with topics can stay in the topics list marked as deleted, with their loaded messages (off by default, a separate setting)
- Chats and channels deleted by the other side, that you were removed from or that you deleted yourself stay in the chats list together with the saved messages, including groups with topics (their topics and the messages loaded in the app are saved too, can be turned off separately); deleting the chat once more removes it for real
- Font customization
- Streamer mode: hides photos, videos, avatars, usernames, IDs, phone numbers and registration dates behind spoilers on every platform, and on Windows and macOS can also hide the whole window from screen capture (both parts are configurable there)
- Local Telegram Premium
- Translator
- Media preview and quick reaction on force click (macOS)
- Enhanced appearance
- App icon picker

See the [AyuGram documentation](https://docs.ayugram.one/desktop/) for the full description of these features.

### From materialgram

- Own Material You themes (Google Day and Google Dark, applied on the first launch)
- **Google Sans** font everywhere (except for Arabic characters, they use the **Vazirmatn** font)
- Material icons instead of default ones (can be turned off in the freshGram appearance settings, restart required)
- Rounded photos and videos and no message bubble tails, plus reverted old paddings (the "Message Rounding and Tail" setting in the freshGram chat settings turns the rounding and tails back to the classic look)
- Colored reply background with an adjustable opacity
- Removed "large emoji" outline
- Reduced use of uppercase in the interface
- Ability to seek round videos
- Ability to delete more than 100 messages at once
- Ability to copy the sticker set author's id and increment
- Ability to mention multiple users at once with right click
- Added admin menu and chat log buttons above the members list
- Copy usernames as @example if possible
- Use photos from @gamee in profile photo list (optional)
- Removed delay when recording voice messages
- Webview platform is reported as "android" (enabled by default, can be turned off in the freshGram settings)
- Replaced all sounds
- Reduced jpeg compression (94-95% on photos, 100% on wallpapers)
- Reduced minimum window size and minimum brush thickness in the photo editor
- Reduced some timeouts (like when opening a chat preview)
- Increased upload speed
- Improved spoiler animation
- Improved sticker pack menu
- Improved chat export (10000 messages in one html document and faster file downloads)
- Improved voice messages bitrate
- Hide your phone number in profile and settings
- Show more recent stickers (unlimited by default)
- Show the approximate date of account creation and the datacenter in profile (optional)
- Show photo/file datacenter and original date
- Show photo platform in media viewer
- Show more info for unique gifts

## Default values

Where AyuGram and materialgram solve the same task, freshGram keeps the AyuGram implementation
and uses the materialgram look as the default value of the corresponding AyuGram setting
(for example, message tails are removed and unlimited recent stickers are enabled by default).

## Links

- Telegram channel: [freshGramDesktop](https://t.me/freshGramDesktop)
- Source code: [Snowy-Fluffy/freshGram](https://github.com/Snowy-Fluffy/freshGram)

## Build instructions

* [Windows 64-bit](docs/building-win-x64.md)
* [macOS](docs/building-mac.md)
* [GNU/Linux using Docker](docs/building-linux.md)

freshGram needs your own Telegram `api_id` and `api_hash`, see [API credentials](docs/api_credentials.md).
Autoupdate is disabled by default because freshGram has no update server.

### GitHub Actions

1. Add the repository secrets `API_ID` and `API_HASH`.
2. Run the `Build environment` workflow once, it builds the library images for x86_64 and aarch64 and pushes them to GitHub Packages.
3. The `Build` workflow builds Linux x86_64 on every push and pull request and produces `freshGram-linux-x86_64.tar.gz`, `freshGram.pacman` (an Arch Linux package, install it with `pacman -U`) and `freshGram.flatpak` (install it with `flatpak install --user freshGram.flatpak`). It builds Linux aarch64 too, its files are `freshGram-linux-aarch64.tar.gz` and `freshGram-aarch64.*` (there is no aarch64 pacman package).
4. The `Build Windows` and `Build macOS` workflows run on every push and manually, and produce `freshGram.exe` (with `freshGram-windows-portable.zip`, which also holds the `TelegramForcePortable` folder that keeps all data next to the exe) and `freshGram.dmg` (ad-hoc signed, open it with right click - Open). Windows is built for x64 and ARM64, the ARM64 files are `freshGram-arm64.exe` and `freshGram-windows-arm64-portable.zip`.
   The first run builds all libraries, which can take more than one run: the finished part is cached, so run the workflow again until it passes.
   Run any of the three manually with `release_tag` (for example `v7.2.10`) or push a `v*` tag to publish a release with all the files attached.

See [docs/upstream-merge.md](docs/upstream-merge.md) for the upstream merge procedure.

## Credits

### Telegram clients

- [Telegram Desktop](https://github.com/telegramdesktop/tdesktop)
- [AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop)
- [materialgram](https://github.com/kukuruzka165/materialgram)
- [Kotatogram](https://github.com/kotatogram/kotatogram-desktop)
- [64Gram](https://github.com/TDesktop-x64/tdesktop)
- [Forkgram](https://github.com/forkgram/tdesktop)

### Libraries used

- [JSON for Modern C++](https://github.com/nlohmann/json)
- [SQLite](https://github.com/sqlite/sqlite)
- [sqlite_orm](https://github.com/fnc12/sqlite_orm)
- [androidx sources](https://github.com/androidx/androidx)
- **Qt 6**, **OpenSSL**, **WebRTC**, **FFmpeg**, **Opus**, **OpenAL Soft** and the other libraries listed in the Telegram Desktop repository
- **Vazirmatn font** ([SIL Open Font License 1.1](https://github.com/rastikerdar/vazirmatn/blob/master/OFL.txt))

### Icons

- [Solar Icon Set](https://www.figma.com/community/file/1166831539721848736)

[//]: # (LINKS)
[telegram_api]: https://core.telegram.org
[telegram_proto]: https://core.telegram.org/mtproto
[license]: LICENSE
