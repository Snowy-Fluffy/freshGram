<p align="center">
  <img src="Telegram/Resources/art/icon256.png" width="128" alt="freshGram">
</p>

<h1 align="center">freshGram</h1>

<p align="center">
  A fast, private and endlessly customizable ToS-breaking desktop Telegram client.<br>
  Ghost mode, saved deleted messages, secret chats, on-device tools, full-functional plugins and a fresh Material look.
</p>

<p align="center">
  <a href="https://t.me/freshGramDesktop">Telegram channel</a> ·
  <a href="#features">Features</a> ·
  <a href="README-RU.md">Русский</a>
</p>

---

freshGram is built on [Telegram Desktop](https://github.com/telegramdesktop/tdesktop), the [Telegram API][telegram_api]
and the [MTProto][telegram_proto] secure protocol. It includes everything from
[AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop) and
[materialgram](https://github.com/kukuruzka165/materialgram), and goes far beyond them with its own features.

## Plugins

**freshGram is made to be extended.** Full-functional plugins let you change almost anything: add your own buttons, menus, commands, settings pages and whole windows, read and send messages, react to events and use the network.

- Plugins run isolated from the app and ask for permissions you can review before installing
- Install from plugin files, update and roll back in a couple of clicks

**Plugins can do almost anything. Filter messages, manage chats, draw your own windows and much more.**

**[Plugin documentation](#)**

## Features

Everything from [AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop) and [materialgram](https://github.com/kukuruzka165/materialgram), plus:

### Privacy

- Flexible ghost mode: read chats and stay unseen
- Streamer mode that hides photos, names, IDs and phone numbers, and can hide the window from screen capture
- Peek at a user's exact last seen without Telegram Premium
- Fake passcodes that can log out accounts, clear data or wipe everything
- A customizable lock screen with wipe after failed attempts
- Message screenshots with a privacy mode that blurs names and avatars
- Clear metadata from media before sending
- Safer links, mini apps and chat export

### Deleted messages and history

- Deleted messages and edits are saved in a local encrypted database with automatic backups
- Saved messages keep their reactions, comments and buttons, and can be searched, replied to, forwarded and translated
- Deleted chats, channels and topics stay in your chat list
- Flexible storage settings and a button to wipe everything saved

### Secret chats

- Secret chats right in the main chat list, protected by your local passcode
- Text, photos, files, stickers, GIFs, voice messages and video notes
- Self-destruct timers, encryption key comparison and private notifications

### On-device tools

- Offline translation that keeps your formatting, with automatic language detection
- Translate the message you are writing and media captions, or translate automatically on send
- Voice message and video note transcription
- Text recognition from images in the media viewer
- Everything works on your device

### Stories

- Post stories right from freshGram with video editing, audiences, albums and drafts
- Post from your avatar or by dropping a file onto the stories list

### Chats

- Search messages by type, sender or channel
- Stash a draft and attached files for later
- Delete your own messages for a period, the last N messages or by topic
- Pin any number of chats locally
- Read your archive from the menu
- Mentions and reactions in topics are read per topic

### Look and feel

- Three looks to choose from: Material, Fagram and classic Telegram
- Material You themes, Google Sans font and Material icons
- Rounded photos, videos and message bubbles
- Font customization and a choice of app icons

### And more

- Seek round videos, delete more than 100 messages at once, mention several users with a right click
- Faster uploads and a better chat export
- Unlimited recent stickers
- Account creation date and datacenter in a profile
- Flatpak and Arch Linux packages

## Arch Linux

freshGram is in the AUR, the packages are maintained by [Snowy-Fluffy](https://github.com/Snowy-Fluffy):

- [`freshgram-bin`](https://aur.archlinux.org/packages/freshgram-bin): prebuilt package
- [`freshgram-git`](https://aur.archlinux.org/packages/freshgram-git): built from the latest source

```bash
yay -S freshgram-bin
```

## NixOS

freshGram is available on NixOS through a **community-maintained flake**. It is not related to the official freshGram developers: it simply repackages the official Linux release (x86_64), so use it at your own risk.

Try it without changing your system:

```bash
nix run 'git+https://relay.ngit.dev/npub18eectrgnhcmmp6e0ec6sp3scf4mvs8nh4vk9pxq5hhsuej9y7qrsrv9dm4/freshgram-flake.git?ref=main'
```

Install it into your profile:

```bash
nix profile install 'git+https://relay.ngit.dev/npub18eectrgnhcmmp6e0ec6sp3scf4mvs8nh4vk9pxq5hhsuej9y7qrsrv9dm4/freshgram-flake.git?ref=main#freshgram-latest'
```

A NixOS module and Home Manager setup are described in the [flake repository](https://gitworkshop.dev/npub18eectrgnhcmmp6e0ec6sp3scf4mvs8nh4vk9pxq5hhsuej9y7qrsrv9dm4/relay.ngit.dev/freshgram-flake).

## Links

- Telegram channel: [freshGramDesktop](https://t.me/freshGramDesktop)
- Source code: [Snowy-Fluffy/freshGram](https://github.com/Snowy-Fluffy/freshGram)

## License

The source code is published under GPLv3 with OpenSSL exception, the license is available [here][license].

## Credits

### Telegram clients

- [Telegram Desktop](https://github.com/telegramdesktop/tdesktop)
- [AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop)
- [materialgram](https://github.com/kukuruzka165/materialgram)
- [Kotatogram](https://github.com/kotatogram/kotatogram-desktop)
- [64Gram](https://github.com/TDesktop-x64/tdesktop)
- [Forkgram](https://github.com/forkgram/tdesktop)
- [FAgram Desktop](https://github.com/fagramdesktop/fadesktop)
- [PTelegram Desktop](https://github.com/wrwrabbit/tdesktop)

### Libraries used

- [JSON for Modern C++](https://github.com/nlohmann/json)
- [SQLite](https://github.com/sqlite/sqlite)
- [sqlite_orm](https://github.com/fnc12/sqlite_orm)
- [androidx sources](https://github.com/androidx/androidx)
- **Qt 6**, **OpenSSL**, **WebRTC**, **FFmpeg**, **Opus**, **OpenAL Soft** and the other libraries listed in the Telegram Desktop repository
- **Vazirmatn font** ([SIL Open Font License 1.1](https://github.com/rastikerdar/vazirmatn/blob/master/OFL.txt))

### Icons

- freshGram icon. Special thanks to [huecoder](https://t.me/huecoder)
- [Solar Icon Set](https://www.figma.com/community/file/1166831539721848736)

[//]: # (LINKS)
[telegram_api]: https://core.telegram.org
[telegram_proto]: https://core.telegram.org/mtproto
[license]: LICENSE

