# chrome-alt-tab

Switch between Chrome tabs and ordinary Windows applications with Alt+Tab.

**Experimental Windows 11 utility.** This is a custom switcher styled after Windows 11, with a Chrome extension and a local C++ helper. It does not inject Chrome tabs into Microsoft's native switcher. There is no permanent helper window or taskbar button.

Features:

- One recent-use list across Chrome profiles, tabs and ordinary windows.
- Tab previews captured locally with your permission; live window thumbnails.
- Windows acrylic backdrop, proportional previews, centred rows, application icons and keyboard or mouse selection.
- Native Windows switching remains available with Ctrl+Alt+Tab.

## Install (Windows 11 x64)

1. Download the **Windows x64 ZIP** from [Releases](https://github.com/ChristopherJBarr/chrome-alt-tab/releases), extract it, and double-click **install.cmd**. No admin access, Visual Studio or runtime download is needed.
2. In each Chrome profile, open `chrome://extensions`, enable **Developer mode**, choose **Load unpacked**, and select `%LOCALAPPDATA%\Programs\ChromeAltTab\extension`.
3. Open the extension popup. Enable previews if wanted, then visit web tabs to capture them.

The app starts automatically when you sign in to Windows, and Chrome connects when it opens. The installer copies everything to your user application folder; the downloaded ZIP and extracted download folder can then be deleted. Chrome Web Store installation is not available yet, so the unpacked-extension step is required once per profile.

A cyan/violet **tray icon** near the clock (possibly under **^**) provides Pause, Resume, Help, Uninstall and Quit. **Pause removes the keyboard hook.** Quit stops the app and pauses connected extensions for that Chrome session. To restart after Quit, choose **Resume / reconnect** in each extension popup, or restart Chrome. Windows sign-in also starts the tray app again.

To remove it: **Settings > Apps > Installed apps > Chrome Alt+Tab (preview) > Uninstall**, then remove the extension from each Chrome profile. This removes the app, startup entry, Start menu shortcut, native-host registration and local logs. Your tabs and browsing data remain untouched. To disable sign-in startup without uninstalling, use Windows **Startup apps**.

For updates, extract the new release and run its installer, then reload the extension in each profile. There is no automatic updater. These preview binaries are unsigned: do not disable security controls if Windows or your organisation blocks them.

See [the local setup guide](https://github.com/ChristopherJBarr/chrome-alt-tab/blob/main/installer/QUICKSTART.html) for details, or [development instructions](docs/development.md) to build from source.

## Controls

| Action | Keys |
| --- | --- |
| Cycle forward / backward | Alt+Tab / Alt+Shift+Tab |
| Choose | Release Alt, or click a card |
| Cancel | Esc |
| Navigate cards | Arrow keys; Home / End |
| Close a tab or window | Hover its card and click the close button |
| Temporarily use native Alt+Tab | Ctrl+Alt+F10 |
| Resume this switcher | Ctrl+Alt+F9 |
| Open native sticky switcher | Ctrl+Alt+Tab |

## Privacy and limitations

Tab IDs, titles and optional JPEG previews stay on this computer. There is no analytics, server or network service in the helper. Incognito and non-normal Chrome windows are excluded. Preview images remain in memory and are bounded per profile. Diagnostic logs contain IDs and errors, not page titles, URLs or images; review logs before sharing them.

This is suitable for public experimentation, **not yet a production-stable release**. Window matching can be ambiguous when Chrome windows have identical active titles; visit a tab in each window after connecting. Cached tab previews can be stale. Elevated applications, unusual windows and mixed-DPI/virtual-desktop transitions need more coverage. The UI follows Windows 11 styling but does not reproduce its private renderer, snap groups, accessibility integration or every native interaction. Large lists use pages of 24 cards.

## Games and compatibility

The app uses the documented Windows `WH_KEYBOARD_LL` hook, in its own process. It does not inject DLLs into applications, install a driver, read game memory, or require elevation. Fullscreen foreground windows use native Alt+Tab automatically. This is a convenience feature, **not an anti-cheat compatibility guarantee**. Choose **Quit before launching protected games**; no vendor certification or anti-cheat testing is claimed. Never use this app to bypass restrictions.

## Project

[Architecture](docs/architecture.md) ? [Protocol](docs/protocol.md) ? [Acceptance tests](docs/switcher.md) ? [Contributing](CONTRIBUTING.md) ? [Security](SECURITY.md)

MIT licensed. Not affiliated with Microsoft or Google. The manifest contains a public key solely to give unpacked installations a stable extension ID; it contains no private signing key or personal account credentials.
