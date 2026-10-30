<p align="center">
  <img src="Telegram/Resources/art/icon256.png" width="128" alt="freshGram">
</p>

<h1 align="center">freshGram</h1>

<p align="center">
  Быстрый, приватный и гибко настраиваемый десктопный клиент Telegram, нарушающий ToS.<br>
  Режим призрака, сохранение удалённых сообщений, секретные чаты, инструменты на устройстве, полнофункциональные плагины и свежий Material-дизайн.
</p>

<p align="center">
  <a href="https://t.me/freshGramDesktop">Telegram-канал</a> ·
  <a href="#возможности">Возможности</a> ·
  <a href="README.md">English</a>
</p>

---

freshGram основан на [Telegram Desktop](https://github.com/telegramdesktop/tdesktop), [Telegram API][telegram_api]
и защищённом протоколе [MTProto][telegram_proto]. В него входит всё из
[AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop) и
[materialgram](https://github.com/kukuruzka165/materialgram), а собственных возможностей у него гораздо больше.

## Плагины

**freshGram создан, чтобы его расширять.** Полнофункциональные плагины позволяют изменить почти всё: добавлять свои кнопки, меню, команды, страницы настроек и целые окна, читать и отправлять сообщения, реагировать на события и использовать сеть.

- Плагины работают изолированно от приложения и запрашивают разрешения, которые можно посмотреть перед установкой
- Установка из файлов плагинов, обновление и откат в пару кликов

**Плагины могут почти всё. Фильтровать сообщения, управлять чатами, рисовать свои окна и многое другое.**

**[Документация плагинов](#)**

## Возможности

Всё из [AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop) и [materialgram](https://github.com/kukuruzka165/materialgram), а также:

### Приватность

- Гибкий режим призрака: читайте чаты, оставаясь незамеченным
- Режим стримера, скрывающий фото, имена, ID и номера телефонов, а также окно от захвата экрана
- Точный последний визит пользователя без Telegram Premium
- Ложные код-пароли, которые могут выйти из аккаунтов, очистить данные или стереть всё
- Настраиваемый экран блокировки со стиранием данных после неудачных попыток
- Снимки сообщений с режимом приватности, размывающим имена и аватарки
- Очистка метаданных медиа перед отправкой
- Более безопасные ссылки, мини-приложения и экспорт чата

### Удалённые сообщения и история

- Удалённые сообщения и правки сохраняются в локальной зашифрованной базе с автоматическими бэкапами
- Сохранённые сообщения хранят реакции, комментарии и кнопки, их можно искать, отвечать на них, пересылать и переводить
- Удалённые чаты, каналы и топики остаются в списке чатов
- Гибкие настройки хранилища и кнопка полной очистки сохранённого

### Секретные чаты

- Секретные чаты прямо в основном списке, под защитой локального код-пароля
- Текст, фото, файлы, стикеры, GIF, голосовые и кружки
- Таймеры самоуничтожения, сверка ключей шифрования и приватные уведомления

### Инструменты на устройстве

- Офлайн-перевод с сохранением форматирования и автоопределением языка
- Перевод текста, который вы пишете, и подписей к медиа, либо автоперевод при отправке
- Расшифровка голосовых сообщений и кружков
- Распознавание текста на изображениях в просмотрщике медиа
- Всё работает на вашем устройстве

### Истории

- Публикуйте истории прямо из freshGram: монтаж видео, аудитории, альбомы и черновики
- Публикация через свою аватарку или перетаскиванием файла на список историй

### Чаты

- Поиск сообщений по типу, отправителю или каналу
- Откладывание черновика и вложенных файлов на потом
- Удаление своих сообщений за период, последних N или по топикам
- Любое число локально закреплённых чатов
- Чтение архива из меню
- Упоминания и реакции в топиках прочитываются по каждому топику отдельно

### Внешний вид

- Три стиля на выбор: Material, Fagram и классический Telegram
- Темы Material You, шрифт Google Sans и Material-иконки
- Скруглённые фото, видео и пузыри сообщений
- Настройка шрифтов и выбор иконки приложения

### И не только

- Перемотка кружков, удаление более 100 сообщений за раз, упоминание нескольких пользователей правым кликом
- Быстрее загрузка файлов и лучше экспорт чата
- Неограниченные недавние стикеры
- Дата создания аккаунта и датацентр в профиле
- Пакеты Flatpak и Arch Linux

> Там, где AyuGram и materialgram решают одну задачу, freshGram оставляет реализацию AyuGram,
> а внешний вид materialgram задаёт как значение по умолчанию соответствующей настройки.

## Arch Linux

freshGram есть в AUR, пакеты поддерживает [Snowy-Fluffy](https://github.com/Snowy-Fluffy):

- [`freshgram-bin`](https://aur.archlinux.org/packages/freshgram-bin): готовый собранный пакет
- [`freshgram-git`](https://aur.archlinux.org/packages/freshgram-git): сборка из последних исходников

```bash
yay -S freshgram-bin
```

## NixOS

freshGram доступен на NixOS через **flake, который поддерживает сообщество**. Он не связан с официальными разработчиками freshGram: он просто переупаковывает официальный Linux-релиз (x86_64), поэтому используйте его на свой риск.

Попробовать, не меняя систему:

```bash
nix run 'git+https://relay.ngit.dev/npub18eectrgnhcmmp6e0ec6sp3scf4mvs8nh4vk9pxq5hhsuej9y7qrsrv9dm4/freshgram-flake.git?ref=main'
```

Установить в свой профиль:

```bash
nix profile install 'git+https://relay.ngit.dev/npub18eectrgnhcmmp6e0ec6sp3scf4mvs8nh4vk9pxq5hhsuej9y7qrsrv9dm4/freshgram-flake.git?ref=main#freshgram-latest'
```

Модуль NixOS и настройка Home Manager описаны в [репозитории flake](https://gitworkshop.dev/npub18eectrgnhcmmp6e0ec6sp3scf4mvs8nh4vk9pxq5hhsuej9y7qrsrv9dm4/relay.ngit.dev/freshgram-flake).

## Ссылки

- Telegram-канал: [freshGramDesktop](https://t.me/freshGramDesktop)
- Исходный код: [Snowy-Fluffy/freshGram](https://github.com/Snowy-Fluffy/freshGram)

## Лицензия

Исходный код опубликован под лицензией GPLv3 с исключением для OpenSSL, текст лицензии находится [здесь][license].

## Благодарности

### Telegram-клиенты

- [Telegram Desktop](https://github.com/telegramdesktop/tdesktop)
- [AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop)
- [materialgram](https://github.com/kukuruzka165/materialgram)
- [Kotatogram](https://github.com/kotatogram/kotatogram-desktop)
- [64Gram](https://github.com/TDesktop-x64/tdesktop)
- [Forkgram](https://github.com/forkgram/tdesktop)

### Используемые библиотеки

- [JSON for Modern C++](https://github.com/nlohmann/json)
- [SQLite](https://github.com/sqlite/sqlite)
- [sqlite_orm](https://github.com/fnc12/sqlite_orm)
- [androidx sources](https://github.com/androidx/androidx)
- **Qt 6**, **OpenSSL**, **WebRTC**, **FFmpeg**, **Opus**, **OpenAL Soft** и другие библиотеки из репозитория Telegram Desktop
- Шрифт **Vazirmatn** ([SIL Open Font License 1.1](https://github.com/rastikerdar/vazirmatn/blob/master/OFL.txt))

### Иконки

- Иконка freshGram. Особая благодарность [huecoder](https://t.me/huecoder)
- [Solar Icon Set](https://www.figma.com/community/file/1166831539721848736)

[//]: # (LINKS)
[telegram_api]: https://core.telegram.org
[telegram_proto]: https://core.telegram.org/mtproto
[license]: LICENSE
