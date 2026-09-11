[한국어](../README.md) | [English](../README.en.md)

# 빌드와 자동 릴리스 / Build and release

`.github/workflows/release.yml`은 PR, `main` 푸시, 수동 실행과 `v*` 태그를 처리합니다. 빌드 job의 권한은 read-only이며, 태그 릴리스 job에만 `contents: write`를 부여합니다. `main`은 보호 규칙에 맞게 관리하세요.

- macOS ARM64: `macos-15` + ARM64 Python 3.12
- macOS x86_64: `macos-15-intel` + x64 Python 3.12
- Windows ARM64: `windows-11-arm` + ARM64 Python 3.12, MSVC ARM64로 hidapi 소스 빌드
- Windows x86_64: `windows-2022` + x64 Python 3.12
- Firmware: Linux의 `espressif/idf:v5.5.5` 컨테이너, NimBLE 패치 적용 후 ESP32 빌드와 C sanitizer 테스트

각 앱 job은 Python/Qt 테스트 → PyInstaller → ZIP 압축 해제 → 실행 파일 아키텍처 검사 → 패키지 self-test → PNG 렌더링을 수행합니다. macOS는 `ditto`로 프레임워크 symlink와 실행 권한을 유지합니다. 앱 패키지에는 Python, Qt, HIDAPI가 포함됩니다.

로컬 앱 빌드 / Local native build:

```sh
python -m pip install -r python/requirements-build.txt
python scripts/build_app.py --platform macos-arm64
python scripts/smoke_app.py --platform macos-arm64
```

다른 플랫폼에서는 `macos-x86_64`, `windows-arm64`, `windows-x86_64`를 사용합니다. 현재 Python의 OS/아키텍처가 목표와 다르면 중단합니다. macOS 실행 앱은 ad-hoc 서명되며 Developer ID 공증은 하지 않습니다. Windows는 Authenticode 서명이 없습니다.

## 릴리스 / Release

```sh
git tag -a v0.1.0 -m "First cross-platform release"
git push origin main v0.1.0
```

버전은 새 릴리스에 맞게 바꾸세요. 이미 공개한 태그를 이동하지 마세요. 태그 이름은 `vMAJOR.MINOR.PATCH` 또는 `vMAJOR.MINOR.PATCH-suffix` 형식을 사용합니다. suffix가 있는 버전은 prerelease로 표시합니다.

모든 빌드·테스트 성공 후 5개 ZIP이 정확히 있는지 검사하고 `SHA256SUMS.txt`를 생성합니다. GitHub의 자동 변경 내역을 포함한 draft release를 생성하고, 파일을 모두 업로드한 뒤 공개합니다. 재실행 시 해당 draft만 이어서 처리합니다. 이미 공개된 릴리스의 파일은 덮어쓰지 않습니다.

All four native apps and the ESP32 build/tests must pass before release publication. Five ZIPs plus checksums are uploaded to a draft, then published. Reruns can recover a draft but never overwrite a published release. Tagged builds validate binaries without physical Bluetooth devices; Windows hardware interoperability still requires the procedure in `WINDOWS_COMPATIBILITY.md`.
