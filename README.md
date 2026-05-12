# sayo-obs-plugin

Native OBS Studio source plugin for **Sayo ASR**.

It captures audio from an OBS audio source, resamples it to the selected model settings, streams audio chunks to a Sayo gRPC server, and renders returned transcripts as subtitles via OBS built-in `text_ft2_source`.

## Features

- **Model discovery**: `HealthCheck` fetches available models and fills the Model/Language dropdowns
- **Explicit connect lifecycle**: stream starts only after you confirm settings with **OK/Apply**
- **Disconnect + parameter freeze**: once connected, server-related parameters are disabled until `Disconnect`
- **Subtitle buffer**: rolling buffer with configurable `max_lines` / `max_chars_per_line`, word-based wrapping, UTF-8 safe
- **Text appearance UI**: exposes the `text_ft2_source` font/color/outline/shadow settings (with a few layout options hidden)
- **Subtitle log (optional)**: writes a per-session log file with meta header and `[start ...]` / `[end ...]` markers

## Project layout

```
proto/                 gRPC API: sayo.proto (source of truth)
src/                   C++ plugin sources
scripts/               local helper scripts (deploy to OBS)
CMakeLists.txt         build script (CMake presets)
CMakePresets.json      windows-x64 / ubuntu-x86_64 presets
vcpkg.json             vcpkg manifest (grpc, protobuf, libsamplerate, simde)
```

## Build

### Windows (MSVC)

Requirements:

- Visual Studio Build Tools 2022 (MSVC v143, Windows SDK, ATL)
- CMake 3.28+
- Git
- vcpkg (manifest mode)
- OBS Studio built from sources (needs `obs.lib`)

Environment example (use forward slashes):

```powershell
$env:VCPKG_ROOT = "D:/vcpkg"
$env:VCPKG_DEFAULT_BINARY_CACHE = "D:/vcpkg/bincache"   # optional
$env:OBS_STUDIO_DIR = "D:/Projects/Sayo/Client/obs-studio"
```

OBS build (once):

```powershell
cd D:\Projects\Sayo\Client\obs-studio
cmake --preset windows-x64
cmake --build --preset windows-x64

# optional: build only libobs
cmake --build .\build_x64 --config RelWithDebInfo --target libobs
```

Plugin build:

```powershell
# from Developer PowerShell for VS 2022
cd D:\Projects\Sayo\Client\sayo-obs-plugin
cmake --preset windows-x64
cmake --build --preset windows-x64
```

Build artifacts land in `build_x64/RelWithDebInfo/` (Windows preset).

### Ubuntu / Debian (x86_64)

Packages:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config \
  protobuf-compiler libprotobuf-dev \
  libgrpc++-dev protobuf-compiler-grpc \
  libsamplerate0-dev libsimde-dev \
  libobs-dev
```

Build:

```bash
cd sayo-obs-plugin
cmake --preset ubuntu-x86_64   # or: ubuntu
cmake --build --preset ubuntu-x86_64
```

## Deploy to OBS (Windows)

There is a helper script that copies the built plugin and its runtime DLLs into the OBS plugin folder.

```powershell
cd sayo-obs-plugin
.\scripts\deploy-obs-plugin.ps1 -WithPdb
```

Optional:

- `-ObsRoot "C:\Program Files\obs-studio"` to deploy to an installed OBS
- `-Config Release` to use another build configuration folder

If OBS is running, the plugin DLL can be locked. In that case the script prints `locked ...` and you need to close OBS and run deploy again.

## Usage in OBS

### Add the source

- Add a new source: **`Sayo ASR Text Source`**
- Pick an **Audio source** (e.g. `Desktop Audio`)

### HealthCheck → choose model/language → Connect

- Press **`HealthCheck`**
  - This fetches the model list from the server and populates dropdowns.
  - It does **not** start streaming by itself.
- Select:
  - **Model**
  - **Language**
- Close settings with **OK** (or press **Apply**)
  - Only then the plugin sends `StreamingConfig` and starts streaming audio chunks.

### Disconnect

- Press **`Disconnect`** to stop streaming.
- While connected, server-related settings are disabled to make it clear they are not expected to change dynamically.

## Subtitle log

Enable **`Subtitle log`** to write a separate file into the OBS logs folder.

- If you **disable** the checkbox and press **OK/Apply**, logging is finished (`[end ...]`) and the file is closed.
- If you **enable** it again later, a **new** log file is created for the next connection.

- A log file is opened **only when a new connection is established**
- The file begins with:
  - `[meta]` … settings dumps … `[/meta]`
  - `[start YYYY-MM-DD HH:MM:SS]`
- On disconnect (and on source destroy) it ends with:
  - `[end YYYY-MM-DD HH:MM:SS]`

The filename uses the source name and supports UTF-8 (including Cyrillic).

OBS log folder: **Help → Log Files → Show Log Files**.

## License

GPL-2.0 (same terms as OBS Studio plugins).
