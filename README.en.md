[한국어](README.md) | [English](README.en.md)

# Great Portable Monitor Remote

Control a macOS or Windows computer with a Great Portable Monitor remote.

An ESP32-S3 receives BLE signals from the remote and sends keyboard, mouse and volume input to your computer. Use **Remote Key Mapper** to assign button actions separately for normal and cursor modes. Once saved to the ESP32-S3, your mappings work with the app closed.

```text
Remote ── BLE ── ESP32-S3 ── USB HID ── macOS / Windows
Settings read/write uses the same HID connection as the selected board.
```

## Downloads

Download the file for your platform from the [latest release](https://github.com/alphaorderly/Great-Portable-Monitor-Remote/releases/latest). **On Windows, download and run the single `.exe`.** No Python installation or `_internal` folder is needed. Extract the entire ZIP for macOS apps and firmware.

| Download | Target | App or contents |
|---|---|---|
| `macos-arm64` | Apple Silicon, macOS 13+ | `RemoteKeyMapper.app` |
| `macos-x86_64` | Intel Mac, macOS 13+ | `RemoteKeyMapper.app` |
| `windows-arm64` | Windows 11 ARM64 | `RemoteKeyMapper-windows-arm64.exe` |
| `windows-x86_64` | Windows 11 Intel/AMD 64-bit | `RemoteKeyMapper-windows-x86_64.exe` |
| `firmware-esp32s3` | ESP32-S3 N16R8 | USB HID firmware and flash addresses |

This project supports ESP32-S3 N16R8 only. Use the `firmware-esp32s3` ZIP. The apps are not notarized on macOS or code-signed on Windows, so you may see a security warning when opening them. File checksums are available in the release's `SHA256SUMS.txt`.

## Getting started

1. Install the [firmware](docs/FIRMWARE.md) on an ESP32-S3 and turn on the remote. See that guide for first pairing.
2.  connect USB/OTG to the PC; use the separate UART/COM port for flashing and logs. No PC Bluetooth pairing is needed for S3.
3. Launch the app and choose **기기 찾기 → 연결** (Find devices → Connect) to load the mappings saved on the ESP32-S3.
4. Choose **일반 모드 / 마우스 커서 모드** (Normal / Cursor mode) and edit button actions.
5. Choose **ESP32-S3에 적용·저장** (Apply and save). The app confirms completion after checking the saved settings.

Modifier keys are labeled **Ctrl / Control, Shift, Alt / Option, Win / Command**. To assign a copy shortcut, choose Ctrl+C on Windows or Command+C on macOS.

The app-list button defaults to Command+Tab. For initial Windows setup, select Windows in **설정 메뉴** (Settings menu), click **기본값 불러오기** (Load defaults) for each mode, then save. This changes the app-list button to Alt+Tab. Selecting an OS alone does not change saved mappings.

![Remote Key Mapper](python/gui/keymapper-preview.png)

The app interface is in Korean.

## Features and limitations

ESP32-S3 USB HID hardware interoperability on macOS and Windows is still unverified. The firmware builds and simulated tests are separate from hardware acceptance.

- Assign arrows, letters, digits, symbols, F1–F24, navigation keys, volume, three mouse buttons and keyboard modifiers.
- In cursor mode, use the mouse speed slider to select 0.25–3× (default 1×), then save it to the ESP32-S3.
- Hold Home in cursor mode to freeze the pointer while repositioning your hand. Movement during the hold is discarded; releasing Home resumes from the current pointer position. Normal-mode Home mappings remain available.
- These features require both the updated app and firmware. Existing settings migrate at 1× speed, and cursor-mode Home becomes reserved for pausing movement.
- The cursor button switches modes and cannot be reassigned.
- Input debugging shows remote button presses. On Windows, keyboard and mouse output sent to the computer may not appear in the log.
- The mapper reconnects to the selected device after transport errors and never automatically repeats a settings write.
- **The new connection recovery changes still require macOS and Windows hardware validation. Windows BLE settings writes also remain unverified.** See the [Windows compatibility review](docs/WINDOWS_COMPATIBILITY.md) for details.

## Running from source

Use Python 3.12 from the repository root.

macOS:

```sh
python3 -m venv .venv-gui
.venv-gui/bin/python -m pip install -r python/requirements.txt
.venv-gui/bin/python python/run_keymapper.py
```

Windows PowerShell:

```powershell
py -3.12 -m venv .venv-gui
.venv-gui\Scripts\python.exe -m pip install -r python/requirements.txt
.venv-gui\Scripts\python.exe python/run_keymapper.py
```

On Windows ARM64, you need ARM64 Python and Visual Studio C++ ARM64 build tools to build `hidapi==0.15.0` from source. To use the app without building it, download the Windows `.exe` from the release. No Linux package is provided.

## Development and releases

```sh
# Use your virtualenv Python (Scripts/python.exe on Windows)
.venv-gui/bin/python -m unittest discover -s python/tests -v
.venv-gui/bin/python python/run_keymapper.py --self-test
# In an activated ESP-IDF 5.5.5 terminal
python firmware/scripts/build.py -B build build
# C sanitizer tests: macOS/Linux/WSL, cc and the patched ESP-IDF SDK
python firmware/tests/native/run_tests.py --idf-path /path/to/esp-idf
```

Pushes to `main` and pull requests build and test the apps on all four platforms, build ESP32-S3 firmware and run the C tests. Pushing a `v*` tag publishes the four apps, the ESP32-S3 firmware ZIP and SHA-256 checksums to GitHub Releases once all checks pass. See the [release guide](docs/RELEASING.md) for details.

| Directory | Contents |
|---|---|
| `python/` | Qt GUI, HID configuration protocol, tests |
| `firmware/` | ESP-IDF source, NimBLE patch, C tests |
| `shared/fixtures/` | Shared Python/C protocol fixtures |
| `scripts/`, `.github/workflows/` | Packaging, checks and automated releases |
| `docs/` | Usage, compatibility and release guides |

[Python guide](python/README.md) · [Firmware](docs/FIRMWARE.md) · [Windows review](docs/WINDOWS_COMPATIBILITY.md)

## String macros

The new app and firmware support per-button ASCII strings, uppercase and punctuation, Enter/Tab, randomized inter-character delays and wait steps, and up to 100 repetitions. The **문자열 매크로** tab automatically reads device settings on connection and button selection, preserving per-button drafts across navigation and reconnection. Edit and save there; saved macros run from the remote with the app closed. Use a US QWERTY layout, English input mode, and Caps Lock off. The new USB product name is `USB Keyboard & Mouse`; standard HID compatibility does not guarantee that an application cannot identify automation. See the [macro guide](python/README.md#문자열-매크로).
