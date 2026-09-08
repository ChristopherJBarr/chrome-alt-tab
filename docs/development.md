# Development

## Prerequisites on a fresh Windows machine

1. Windows 11 x64, with Google Chrome 120 or newer.
2. Install Visual Studio 2022 (17.12 or newer), Visual Studio 2026, or their Build Tools. Select **Desktop development with C++**, MSVC x64/x86 tools, **Windows 11 SDK 10.0.26100.0 or newer**, and **C++ CMake tools for Windows** (includes Ninja). The Visual Studio installer provides these components; there is no C++ package manager dependency.
3. For tests, install Node.js 22 or newer and Python 3.11 or newer, and make `node` and `python` available on PATH. No npm or pip packages are needed.
4. Open **Developer PowerShell for VS** from the Start menu, and change to the extracted or cloned repository folder. If using a developer command prompt, start `powershell` from it for the commands below.

## Configure, build, test

```powershell
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
node --test tests/extension.test.mjs tests/background.test.mjs tests/previews.test.mjs
python tests/switcher_integration.py build-release/native/chrome-alt-tab-switcher.exe
```

The integration test starts a broker without a keyboard hook and uses synthetic profiles. Use a separate build directory from your active installation: it refuses to share an already-running broker. Run tests from the repository root. Repeat with a separate `build-debug` directory and `-DCMAKE_BUILD_TYPE=Debug` for Debug validation.

Alternatively, use the installed Visual Studio generator (example for VS 2022):

```powershell
cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64
cmake --build build-vs --config Release --parallel
ctest --test-dir build-vs -C Release --output-on-failure
.\scripts\register-native-host.ps1 -BuildDirectory build-vs -Configuration Release
```

## Connect Chrome

```powershell
.\scripts\register-native-host.ps1 -BuildDirectory build-release
```

Registration writes one per-user Chrome Native Messaging registry entry and a generated `build/native-host.json`. Administrator access is unnecessary. The script derives the extension ID from the manifest's public key and resolves the executable relative to this checkout. It refuses to overwrite a different checkout's registration. If PowerShell policy blocks scripts, follow your organisation's policy; the project does not change execution policy.

Open `chrome://extensions` in each profile, enable Developer mode, choose Load unpacked and select `extension`. Open the popup to connect. Enable previews only if wanted, then visit a normal HTTP(S) page in each Chrome window. There is no extension build step.

Native code changes require rebuilding the executable after its processes have exited. Choose Quit in the tray (or run the executable with `--quit`), rebuild, then reconnect using each extension popup. The tray app now stays running while Chrome is closed. Extension changes require clicking Reload in each profile's extensions page.

## Rendering and manual checks

`chrome-alt-tab-switcher.exe --visual-test` opens the current design with ordinary windows and three synthetic cards, without installing a keyboard hook. It uses the same hidden-owner arrangement and window styles as the live switcher, including its exclusion from Alt+Tab and the taskbar. Close it with Alt+F4. Use an isolated build directory for this test. `--demo` instead enables the keyboard hook with synthetic tabs; use Ctrl+Alt+F10 to suspend it.

Follow [acceptance checks](switcher.md) before distributing a binary. Automated tests do not prove Windows foreground policy or physical held-key behaviour.

## Sharing source

```powershell
.\scripts\package-source.ps1
```

This creates `dist/chrome-alt-tab-source.zip` from an explicit source allowlist. It excludes build caches, native-host registration, binaries, logs and Git metadata. Inspect its contents before publishing. Keep the generated `dist` and `build*` folders out of Git. No repository is created or uploaded by this script.

For backdrop checks, use Ctrl+Alt+F8 to hold the **Chrome-connected** switcher open over a contrasting window from another application. Escape dismisses it. A standalone visual test alone does not establish that the background works in the Chrome-launched helper.

## Package a binary release

```powershell
.\scripts\package-release.ps1 -BuildDirectory build-release
```

The ZIP contains the statically linked Release executable, extension, per-user installer/uninstaller, offline quick-start guide and a SHA-256 payload manifest. No MSVC redistributable is needed. The accompanying `.sha256` checks download integrity, not publisher identity. No signing certificate is currently configured.

The installer writes only to `%LOCALAPPDATA%\Programs\ChromeAltTab`, the user's Start menu, and HKCU native-host, Run and Uninstall keys. Installation wrappers use a process-scoped PowerShell execution-policy option; they do not modify the machine's saved policy and do not override enterprise policy. Uninstallation resolves and validates the exact installation directory before deleting it, rejects reparse points, and only removes matching registrations and shortcuts.

For a developer registration, removal remains `scripts/register-native-host.ps1 -Unregister`, then Quit the tray and remove the unpacked extension. The release installer refuses to overwrite an unrelated registration.

Release builds define `WINRT_NO_SOURCE_LOCATION` so C++/WinRT does not embed developer source paths. Only Release executables are distributed; Debug PDBs and build artifacts remain local.

CI also checks the distribution for embedded build paths and runs `tests/install_lifecycle.ps1` on a clean disposable Windows account. That test refuses to run over an existing installation or development registration.
