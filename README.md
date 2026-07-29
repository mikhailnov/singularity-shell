# singularity-shell

Неофициальное десктопное приложение менеджера задач [Singularity](https://singularity-app.ru/) на **QtWebEngine**.

Их официальное приложение сделано на Electron и доступно только через магазин Snap.

Это же неофициальное приложение использует системный QtWebEngine, может быть собрано из исходников и установлено из репозитория дистрибутива.

Переиспользуется оффлайн-версия на javascript из snap-пакета. 

![Inbox](docs/img-ru/inbox.png)

![Sync status](docs/img-ru/sync-status.png)

![Bootstrap](docs/img-ru/bootstrap.png)

![Zoom menu](docs/img-ru/menu-zoom.png)

![File menu](docs/img-ru/menu-file.png)

![Diagnostics menu](docs/img-ru/menu-diagnostics.png)

## Установка

### ROSA Linux, МОС, МосТех.ОС

На Росе 13+: `sudo dnf install singularity-shell`  
(Роса Фреш, Роса Хром)

Исходники RPM: https://abf.io/import/singularity-shell

### Сборка из исходников

Установите deve-пакеты Qt и QtWebEngine.

```bash
mkdir build
cd build
cmake ..
make -j$(nproc)
./singularity-shell
```

## Как это работает

Официальный клиент Electron запускает приложение на привилегированном
кастомном источнике `sg://renderer`, раздаваемом с локального диска. Эта оболочка
в точности повторяет схему с помощью `QWebEngineUrlScheme` + `QWebEngineUrlSchemeHandler`:
источник оболочки всегда «в сети», потому что ресурсы на диске, поэтому **холодный
старт работает без интернета**; синхронизация и вход идут между источниками в
облако вендора (его API отвечает `Access-Control-Allow-Origin: *`), как в
официальном клиенте.

- Базовые ресурсы могут поставляться в `/usr/share/singularity-shell/` →
  первый запуск работает без интернета «из коробки».
- **Фоновый updater** раз в сутки проверяет Snap Store, загружает новые
  ресурсы в `$XDG_DATA_HOME/singularity-shell/assets/` (проверка SHA3-384),
  ставит их через атомарную замену symlink — применяются **при следующем
  запуске**. Отключается флагом `--no-auto-update`.
- Всё состояние (cookies, IndexedDB, service worker) хранится в постоянном
  `QWebEngineProfile` в `$XDG_DATA_HOME/singularity-shell/profile/`.

### Известные проблемы

- **Печать (Ctrl+P)**: печать плана дня не работает (подробнее в `docs/printing.md`).

## Архитектура: превращаем Electron-приложение в QtWebEngine

Этот раздел написан как переиспользуемая инструкция для тех, кто делает
похожую миграцию. Здесь описан общий подход, процесс обратной разработки
конкретного вендора и все неочевидные грабли, в которые мы наступили.

### Общая схема

Десктопное Electron-приложение обычно состоит из:

1. **Кастомной URL-схемы** (`sg://renderer` в нашем случае), раздаваемой
   с локального диска — это и есть весь интерфейс приложения.
2. **Preload-скрипта**, внедряемого на каждую страницу и выставляющего
   доступ к API рабочего стола (`window.preloadApi`) через `contextBridge`.
3. Облачных API-вызовов кросс-ориджинно с кастомной схемы к серверам вендора.

Чтобы воспроизвести это на QtWebEngine, нужно:

| Electron | QtWebEngine |
|---|---|
| `protocol.registerSchemesAsPrivileged` | `QWebEngineUrlScheme::registerScheme` |
| `protocol.handle` | `QWebEngineUrlSchemeHandler` |
| `contextBridge.exposeInMainWorld` | `QWebEngineScript` на `DocumentCreation` + `QWebChannel` |
| `BrowserWindow` | `QMainWindow` + `QWebEngineView` |
| Постоянная `session` | Именованный `QWebEngineProfile` + `setPersistentStoragePath` |
| `autoUpdater` | Свой `UpdateController`, опрашивающий Snap Store API |

### Обратная разработка preload API вендора

Самая трудоёмкая часть — воспроизведение поверхности `window.preloadApi`.
Подход:

1. **Скачать snap вендора** через публичный Snap Store API (snapd не нужен —
   `.snap` это просто SquashFS-архив).
2. **Извлечь `resources/app.asar`** (архивный формат Electron) с помощью
   собственной утилиты (`tools/asar-extract.cpp` — без зависимостей, ~200
   строк на C++).
3. **Прочитать `build/main/preload.js`** — preload-скрипт вендора. Он
   определяет класс, создающий объекты контроллеров через хелпер `ipcService`.
   Нужно воспроизвести имя каждого контроллера и сигнатуру каждого метода.
4. **Прогрепать `build/js/app.bundle.js`** на предмет `preloadApi.` для
   подтверждения, какие контроллеры и методы реально вызываются во время работы.

Структура preload вендора (восстановлена из v12.6.0):

```
preloadApi
├── ipcRenderer          { send, on, off }
├── isPopup              bool
├── windowController     { minimize, maximize, close, isMaximized, getId,
│                          isVisible, isFocused, isFullScreen, hide, show,
│                          focus, blur, setAlwaysOnTop, moveTop,
│                          setFullScreen, OPEN_NEW_WINDOW, … }
├── zoomController       { ZOOM_IN, ZOOM_OUT, ZOOM_RESET }
├── urlController        { openExternal, openPath, supportsOpenPath }
├── appController        { QUIT_APP, RESTART_APP, … }
├── fetchController      { fetch }          ← проксирует нативный fetch()
├── updateController     { checkUpdates, applyUpdateAndRestart }
├── menuController       { TOOLBAR_UPDATED, POPUP_MENU, … }
├── popupController      { open, close, sendResult, … }
├── …и ещё ~10 контроллеров (все заглушены)
└── windowRenderToMainBridge  ← расширяет windowController, добавляя
     addListener(), getPosition(), getBounds(), id
```

### Грабли и хаки

#### 1. Схему `sg://` НЕЛЬЗЯ делать «local»

Установка `LocalScheme | LocalAccessAllowed` на кастомной схеме заставляет
Chromium обращаться с ней как с `file://` — он запрещает ЛЮБОЙ fetch/XHR к
удалённым http(s)-источникам. Каждый облачный вызов мгновенно падает с
`TypeError: Failed to fetch` **до** CORS, до отправки хотя бы одного пакета.
В DevTools Network — пусто. Это убивало весь трафик синхронизации, а диагностика заняла дни.

Правильные флаги: `SecureScheme | ServiceWorkersAllowed | CorsEnabled |
FetchApiAllowed` — ровно то, что использует код Electron у вендора.

#### 2. Заголовок `deadline` в gRPC-web вендора

gRPC-web клиент вендора шлёт кастомный заголовок `deadline`. Их nginx в
CORS preflight-ответе НЕ включает его в `Access-Control-Allow-Headers`
(верно для ВСЕХ источников, включая официальный веб-клиент). Десктопный
клиент Electron обходит это, отправляя API-вызовы из главного процесса
(Node axios, без CORS). У нашего рендерера такой возможности нет.

Исправление: `VendorApiInterceptor` удаляет этот единственный заголовок
для хостов `*.singularity-app.com/.ru` до проверки CORS. Хирургически —
без `--disable-web-security`. Заголовок лишь рекомендательный клиентский
таймаут, семантически безопасно его выбросить.

#### 3. Сериализация аргументов QWebChannel требует явного `String()`

При вызове C++ слота через JS-прокси QWebChannel аргумент должен быть
**явным** JavaScript String. Передача переменной, содержащей строку (даже
если `typeof` говорит `"string"`), может привести к пустому вызову на
стороне C++. Всегда используйте `b.slotName(String(jsVariable))`.

#### 4. Каждому окну — свой экземпляр PreloadBridge

PreloadBridge испускает сигналы (minimizeRequested, closeRequested и др.).
Один мост на несколько окон приводит к перекрёстному засорению сигналов:
минимизация одного окна дёргала все. Каждое всплывающее окно, созданное через
`createWindow()`, получает свежий `PreloadBridge`, привязанный к своему
`QWebEngineView`.

#### 5. `windowRenderToMainBridge` должен расширять windowController

Вендорский `windowBridgeFactory` расширяет объект ВСЕМИ методами
windowController плюс добавляет `getPosition()`, `getBounds()`,
`addListener(channel, cb)` и `id`. Приложение обращается к ним через
`this.provider.window.focus()` и т.п. Голая заглушка `{send, on}`
вызывает `TypeError: e.addListener is not a function`.

#### 6. Service Worker и кастомные схемы

SW вендора регистрируется на `sg://renderer` и перехватывает события запросов
для источника оболочки. Проверено: после первого запуска SW активен, IndexedDB
содержит `AppDatabase` + `SingularityLogs3`; оба сохраняются между
перезапусками. Для локальной раздачи файлов перехват SW — чистый оверхед,
но **не удаляйте его** — SW также управляет офлайн-очередью синхронизации.

#### 7. Окна OAuth должны оставаться внутри приложения

Процесс входа идёт через `accounts.google.com`, `appleid.apple.com`,
`login.microsoftonline.com`. Эти хосты ОБЯЗАНЫ оставаться в приложении
(общие cookies через постоянный профиль) для завершения аутентификации
через `window.opener`. Все остальные внешние URL открываются в системном
браузере через `acceptNavigationRequest` → `QDesktopServices::openUrl`.
Попапы, созданные для `target=_blank`, автоматически закрываются после
делегирования в системный браузер.

#### 8. `QJsonDocument::fromVariant(QString)` даёт некорректный JS

Используется для инъекции текста статуса на bootstrap-страницу.
`QJsonDocument::fromVariant(text)` на чистом QString в некоторых версиях
Qt возвращает null-документ; `toJson()` тогда возвращает пустоту, создавая
битый `<script>`. Исправление: обернуть в `QVariantList{QVariant(text)}` →
получается `["правильно заэкранированный текст"]` → обрезаем скобки.

#### 9. `qt_standard_project_setup()` затирает правленые вручную .ts-файлы

Когда `LinguistTools` указан в `find_package`, `qt_standard_project_setup()`
автоматически находит `.ts`-файлы в `translations/` и запускает `lupdate`,
затирая ручные переводы пустыми шаблонами. Исправление: хранить `.ts` в
нестандартной директории (`i18n/`).

#### 10. Скрытие встроенных оконных кнопок вендора

Окно Electron у вендора без рамки (frameless) и рисует собственные
кнопки «свернуть / развернуть / закрыть» прямо в веб-контенте (класс
`.win-top-panel`). Поскольку Qt даёт нативные оконные декорации, эти
внутристраничные кнопки избыточны и отъедают место по вертикали.
Одно CSS-правило при создании страницы их скрывает:
`.win-top-panel { display: none !important; }`.

## Возможности

### Фон страницы под системную тему

При запуске фон страницы соответствует системной цветовой схеме:
тёмный `#1a1a2e` или светлый `#f0f0f5`. Меняется на лету при
переключении темы (`QStyleHints::colorSchemeChanged`). Убирает
раздражающую белую вспышку до загрузки контента.

### Сохранение масштаба

Масштаб (Ctrl+= / Ctrl+- / Ctrl+0) сохраняется в
`~/.local/share/singularity-shell/settings.conf` и восстанавливается
при следующем запуске. Меню **Масштаб** показывает текущий процент
и предлагает действия Увеличить / Уменьшить / Сбросить.

Клавиатурные сокращения обрабатываются через `eventFilter` на
`QWebEngineView` — внутренняя обработка клавиш Chromium
перехватывает `QShortcut` и `QAction` на более низком уровне Qt.

## Структура кода

```
src/            Приложение на C++ (см. qt-tz.md §6.1)
tools/          asar-extract.cpp — распаковщик asar без зависимостей
scripts/        fetch-assets.sh — snap → проверенные ресурсы, без snapd
resources/      bootstrap-страница (заглушка первого запуска, FR-10)
packaging/      .desktop-файл, RPM-спецификация
i18n/           Русский перевод (Qt Linguist)
docs/           printing.md — почему не работает печать плана дня
```

## Расположение данных

| Путь | Содержимое |
|---|---|
| `/usr/share/singularity-shell/assets/` | Базовые ресурсы из RPM (только чтение) |
| `~/.local/share/singularity-shell/assets/` | Ресурсы фонового обновления (версионированы, symlink `current`) |
| `~/.local/share/singularity-shell/profile/` | Cookies, IndexedDB, SW, кеш |
| `~/.local/share/singularity-shell/settings.conf` | Состояние UI, масштаб, временные метки updater |
