# Native Messaging protocol v1

Chrome starts `org.chrome_alt_tab.host`. Each connection represents one profile. Numeric tab IDs are local to that connection. Transport is a four-byte little-endian byte length followed by UTF-8 JSON; stdout is exclusively protocol data. Messages include `version: 1`. The broker limits frames to one MiB and disconnects malformed peers.

## Handshake and snapshots

The broker sends `ready` with `hostVersion: "0.5.1"` and `ownerMode: "custom-switcher"`. The extension then sends a complete snapshot:

```json
{
  "type": "snapshot",
  "version": 1,
  "focused": true,
  "activeTabId": 42,
  "tabs": [
    {"id": 42, "windowId": 8, "title": "Example", "lastAccessed": 1000}
  ]
}
```

Only normal, non-incognito tabs are mirrored. IDs are nonnegative signed 32-bit integers; `activeTabId` must be listed or -1. Titles are limited to 1024 UTF-16 code units without embedded nulls. A profile may contain at most 2000 tabs. Access timestamps must be finite, nonnegative epoch milliseconds and no more than one minute in the future. Missing timestamps default to zero. Snapshots without `focused` receive a reload diagnostic and cannot enable interception.

A valid update receives `synced` with `count` and `activeTabId`. The broker reconciles removals and seeds new MRU entries; background snapshots, title changes and preview refreshes do not count as visits. No URLs are transmitted in snapshots.

## Activation

`activate` includes `tabId`. The extension rechecks that the tab still belongs to a normal non-incognito window, selects it and focuses that Chrome window. Commands execute serially, followed by `result` with boolean `ok` and a fresh snapshot. Success acknowledges Chrome API completion; it does not prove Windows foreground activation. The broker keeps a bounded, ordered pending-activation queue per profile and permits a short-lived foreground handoff for the newest selection only.

The extension also understands scoped `close`, `paused` and `error` messages, and exposes `close` through the card close button. It offers no arbitrary script, URL-opening or filesystem command.

## Previews

The handshake advertises `previewState: true`. Only with that capability does the extension send `preview_state` with boolean `enabled`, after checking preview permission and when permission changes. This state belongs to one profile. Disabled state clears its images and displays "Enable previews in the extension". Enabled or unknown state with no image displays a large application icon. An older extension remains compatible and is treated as unknown until reloaded; absence of an image never implies that permission was denied.

`preview` includes a mirrored `tabId` and `image` containing base64 JPEG. WIC validates and fully decodes at most 128 KiB and 640 by 400 pixels. The broker retains at most 48 images per profile and acknowledges `previewed` with `tabId`. Absent tab IDs or invalid images disconnect the peer. `clear_previews` removes all images, or the supplied `tabId`; it has no acknowledgement.

The extension captures only after optional permission is granted, for a focused normal HTTP(S) tab. Navigation and permission revocation clear cached images. Images are not logged or stored on disk.

## Lifecycle and tests

EOF removes the profile's cards and previews. The production broker remains available in the tray with no clients. Quit sends `paused` before disconnecting; the extension stops reconnecting for that Chrome session. The extension uses bounded reconnect backoff and session-scoped pause state. A stale port cannot issue commands in a replacement connection.

The `--test-broker` process installs no keyboard hook and accepts synthetic inspection/selection commands used by `tests/switcher_integration.py`. Normal broker mode rejects those commands. The integration suite exercises framing rejection, profile isolation, MRU stability, JPEG handling and popup cleanup. No browser extension sends test commands.
