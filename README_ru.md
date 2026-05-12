# sayo-obs-plugin

Нативный source‑плагин для OBS Studio для **Sayo ASR**.

Плагин захватывает аудио из выбранного аудио‑источника OBS, ресэмплит его под параметры выбранной модели, отправляет аудиочанки на gRPC‑сервер Sayo и выводит полученный текст субтитрами через встроенный `text_ft2_source`.

## Возможности

- **Получение моделей**: `HealthCheck` запрашивает доступные модели и заполняет выпадающие списки Model/Language
- **Явный цикл подключения**: стрим стартует только после подтверждения настроек через **OK/Apply**
- **Disconnect + заморозка параметров**: после подключения серверные параметры блокируются до `Disconnect`
- **Буфер субтитров**: “роллинг” буфер с `max_lines` / `max_chars_per_line`, перенос по словам, безопасная обработка UTF‑8
- **Настройки внешнего вида текста**: доступны параметры `text_ft2_source` (шрифт/цвет/обводка/тень; часть layout‑опций скрыта)
- **Subtitle log (опционально)**: лог в отдельный файл на сессию с meta‑заголовком и маркерами `[start ...]` / `[end ...]`

## Структура проекта

```
proto/                 gRPC API: sayo.proto (источник правды)
src/                   исходники плагина (C++)
scripts/               локальные скрипты (деплой в OBS)
CMakeLists.txt         сборка (CMake presets)
CMakePresets.json      пресеты windows-x64 / ubuntu-x86_64
vcpkg.json             vcpkg manifest (grpc, protobuf, libsamplerate, simde)
```

## Сборка

### Windows (MSVC)

Требуется:

- Visual Studio Build Tools 2022 (MSVC v143, Windows SDK, ATL)
- CMake 3.28+
- Git
- vcpkg (manifest mode)
- OBS Studio, собранный из исходников (нужен `obs.lib`)

Пример переменных окружения (используй прямые слэши `/`):

```powershell
$env:VCPKG_ROOT = "D:/vcpkg"
$env:VCPKG_DEFAULT_BINARY_CACHE = "D:/vcpkg/bincache"   # опционально
$env:OBS_STUDIO_DIR = "D:/Projects/Sayo/Client/obs-studio"
```

Сборка OBS (один раз):

```powershell
cd D:\Projects\Sayo\Client\obs-studio
cmake --preset windows-x64
cmake --build --preset windows-x64

# опционально: собрать только libobs
cmake --build .\build_x64 --config RelWithDebInfo --target libobs
```

Сборка плагина:

```powershell
# из Developer PowerShell for VS 2022
cd D:\Projects\Sayo\Client\sayo-obs-plugin
cmake --preset windows-x64
cmake --build --preset windows-x64
```

Артефакты сборки (Windows preset): `build_x64/RelWithDebInfo/`.

### Ubuntu / Debian (x86_64)

Пакеты:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config \
  protobuf-compiler libprotobuf-dev \
  libgrpc++-dev protobuf-compiler-grpc \
  libsamplerate0-dev libsimde-dev \
  libobs-dev
```

Сборка:

```bash
cd sayo-obs-plugin
cmake --preset ubuntu-x86_64   # или: ubuntu
cmake --build --preset ubuntu-x86_64
```

## Установка в OBS (Windows)

Есть вспомогательный скрипт, который копирует собранный плагин и runtime DLL‑зависимости в папку плагинов OBS.

```powershell
cd sayo-obs-plugin
.\scripts\deploy-obs-plugin.ps1 -WithPdb
```

Опционально:

- `-ObsRoot "C:\Program Files\obs-studio"` — деплой в установленный OBS
- `-Config Release` — использовать другую конфигурацию сборки

Если OBS запущен, `sayo_obs_plugin.dll` может быть заблокирован. В этом случае скрипт печатает `locked ...` — закрой OBS и запусти деплой ещё раз.

## Использование в OBS

### Добавить источник

- Добавь новый источник: **`Sayo ASR Text Source`**
- Выбери **Audio source** (например `Desktop Audio`)

### HealthCheck → выбрать Model/Language → Connect

- Нажми **`HealthCheck`**
  - Список моделей запрашивается с сервера и заполняет dropdown’ы.
  - Сам по себе `HealthCheck` стрим не запускает.
- Выбери:
  - **Model**
  - **Language**
- Закрой окно настроек через **OK** (или нажми **Apply**)
  - Только после этого отправляется `StreamingConfig` и начинается отправка аудиочанков.

### Disconnect

- Нажми **`Disconnect`**, чтобы остановить стрим.
- Пока соединение активно, серверные параметры заблокированы.

## Subtitle log

Включи **`Subtitle log`**, чтобы писать отдельный файл в папку логов OBS.

- Если снять галочку и нажать **OK/Apply**, запись завершается (`[end ...]`), файл закрывается.
- Если включить снова позже, для следующего подключения будет создан **новый** файл.

- Файл открывается **только при установлении нового соединения**
- В начале файла:
  - `[meta]` … дампы настроек … `[/meta]`
  - `[start YYYY-MM-DD HH:MM:SS]`
- При отключении (и при уничтожении источника) в конце:
  - `[end YYYY-MM-DD HH:MM:SS]`

Имя файла использует имя источника и поддерживает UTF‑8 (включая кириллицу).

Папка логов OBS: **Help → Log Files → Show Log Files**.

## Лицензия

GPL-2.0 (как у OBS Studio plugins).

