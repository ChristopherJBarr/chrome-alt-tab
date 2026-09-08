# Architecture

```text
Chrome profiles (Manifest V3 extension)
    | Native Messaging: framed JSON over stdio
One bridge process per profile
    | restricted local named pipe
One broker per user / Windows session / executable path
    | keyboard hook, Win32 popup, DWM thumbnails
Combined Windows-and-Chrome-tab switcher
```

The broker owns an always-hidden owner window and a popup shown only during switching. Neither has a taskbar button, and does not use WindowTabManager. The low-level keyboard hook runs on a dedicated message-loop thread. A UI heartbeat allows hook input to pass through when the broker stalls. Ctrl+Alt+F10 suspends interception; Ctrl+Alt+F9 resumes it.

The recent-use model shares one clock across ordinary window activations and observed Chrome tab visits. Initial browser timestamps seed new tabs once. Snapshot refreshes and image updates do not manufacture visits. The visible list freezes while cycling. Chrome windows with known tab mappings are replaced by their tab cards; unrecognised windows remain ordinary entries.

Each profile has its own tab namespace, previews and activation queue, even when Chrome reuses numeric tab IDs across profiles. Window mappings are learned from observed foreground Chrome windows and active titles, checked against the Chrome ancestor process. After a restart, selection can recover a missing mapping from a unique matching window title in that verified process, using the selected tab or its last active sibling. Titles shared across profiles or browser windows are not used for recovery. A known window is brought forward before the extension receives the activation command. Identical titles remain an ambiguity; this is an explicit release limitation. An activation acknowledgement permits a bounded foreground handoff only for the newest selection, a matching Chrome process and no intervening application activation.

Preview capture is optional. The extension captures visible normal HTTP(S) tabs after a visit, rate-limits and scales JPEGs; the broker validates and decodes them using WIC. Ordinary application windows use DWM live thumbnails. Images are cached in memory, at most 48 per profile. The renderer uses DPI-scaled, aspect-ratio-aware cards and Windows 11 rounded decoration with the documented Desktop Acrylic backdrop. A 32-bit surface preserves transparency around opaque cards; the OS controls the material and an opaque fallback is used when unavailable or in high contrast. It reads the system light/dark preference when opening.

## Why a custom switcher?

The initial experiment tested Windows.UI.Shell.WindowTabManager. A separate helper owning the tab group introduced an unwanted owner window. The cross-process Chrome-window ownership probe returned E_ACCESSDENIED. Hiding the owner broke reliable activation; cloaking did not satisfy the no-helper requirement. These are results from the development machine, not a claim that every Windows build behaves identically.

The obsolete probes and shell-host implementations have been removed. The custom switcher is an explicit architecture choice, rather than a silent fallback when a native API fails. It does not hook into, patch or inject code into Chrome or Explorer.

## Trust boundaries

Named-pipe and singleton names include the user's SID, Windows session and executable-path hash. The pipe rejects remote clients; its ACL restricts it to the current user. Both endpoints verify the peer executable path. The native bridge validates Chrome ancestry and retains a process handle. These checks reduce accidental cross-application access; they are not a security boundary against arbitrary malicious code already running as the same user.

Bounds include eight profiles, 2,000 tabs per profile, one MiB frames, 128 KiB/640-by-400 JPEGs, finite outgoing queues and stalled-transfer timeouts. A malformed client is disconnected without taking healthy profiles down. The helper does not perform network requests.

## Native snap groups

The custom switcher does not yet expose Explorer's snap groups. The documented [IsWindowArranged API](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-iswindowarranged) reports whether one window is snapped, not the shell's group identity, membership or group MRU. No supported enumeration/activation contract for those groups was found in the Windows SDK and Microsoft documentation reviewed for this implementation. Inferring groups from adjacent rectangles can join unrelated windows and miss minimised members, so that heuristic is not shipped. Ctrl+Alt+Tab continues to open the genuine native switcher, including its groups.

The native look remains a close approximation. Cards follow preview aspect ratios instead of a uniform grid; rows are centred, with smaller previews on constrained displays. Native snap-group cards, full UI Automation accessibility, native animations and pixel-identical rendering remain gaps. Closing a card dismisses the popup before delivering a scoped Chrome close command or a normal WM_CLOSE request, allowing the target application to present any save prompt.

## Frame presentation

The switcher prepares its first off-screen frame and DWM thumbnail registrations before showing the popup. Default window transitions are disabled to avoid exposing an intermediate popup surface. Live thumbnails stay registered throughout selection changes within a page; rebuilding is reserved for opening a new list or changing pages. Selection, hover and preview updates invalidate the affected cards only.

A reusable 32-bit DIB backs the rendered page. GDI work is explicitly flushed before CPU alpha processing, as required by [CreateDIBSection](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-createdibsection). This prevents deferred GDI writes from overwriting prepared glass transparency. The popup is hidden before its thumbnail surfaces are released. Regression checks cover hidden-frame preparation, stable thumbnail registrations, reusable buffers and preview refreshes while cycling.

The popup retains `WS_CAPTION | WS_THICKFRAME` and removes visible window chrome through `WM_NCCALCSIZE` and `WM_NCHITTEST`. A permanently hidden owner keeps it out of Alt+Tab and the taskbar without an additional visible helper window.

The background uses Windows.UI.Composition's documented [CreateHostBackdropBrush](https://learn.microsoft.com/en-us/uwp/api/windows.ui.composition.compositor.createhostbackdropbrush), enabled through `DWMWA_USE_HOSTBACKDROPBRUSH`. A system-backdrop attribute alone accepted the request but still produced a flat surface in the Chrome-launched process. Testing a separate executable with another visible window in the same process did not reproduce that failure. Background verification therefore uses the actual Chrome-connected helper over a contrasting window in a separate process, including repeated opening and closing.

A Direct2D composition surface presents the existing GDI-rendered cards above the host brush. The bitmap and surface are reused until page dimensions change; only dirty regions are uploaded. A light premultiplied tint (20% in dark mode, 40% in light mode) keeps the blurred background visible between cards, while native DWM thumbnails remain live. Windows controls the host brush's transparency according to user settings and power policy. Battery Saver, high contrast, or Windows advanced effects being disabled use the opaque themed rendering path. These conditions are checked whenever the switcher opens, and the fallback reason is logged. This avoids the host brush becoming a black background and avoids allocating a GPU composition surface for an opaque-only session. No desktop screenshot is read back, saved or transmitted for this effect.

Ctrl+Alt+F8 holds the live switcher open for visual diagnostics. Escape cancels, Enter chooses, and clicking another window dismisses it. This optional shortcut does not replace native Ctrl+Alt+Tab. The real Alt+Tab path and the diagnostic shortcut share the same renderer.

## Installed lifecycle

A per-user tray application owns the broker; no Windows service is used. HKCU Run starts it at sign-in. The same statically linked GUI-subsystem executable handles Native Messaging using inherited standard pipes, so Chrome connections do not open a console. Only the interactive session owns the keyboard hook. Pause removes that hook; Resume recreates it. Quit first sends each connected extension a session-scoped pause message, then releases all hooks and exits. Chrome startup explicitly wakes the extension and clears its session pause. Fullscreen foreground windows pass Alt+Tab through without invoking the custom UI; this does not establish compatibility with anti-cheat software.

Local named control events use the same user/session/executable-path identity and restricted ACL as the broker. `--pause`, `--resume` and `--quit` target only that installation. The tray re-registers its icon after Explorer broadcasts TaskbarCreated. Uninstall is available in the tray and Windows Installed apps.
