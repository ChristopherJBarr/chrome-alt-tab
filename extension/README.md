# Chrome extension

Load this folder unpacked in each Chrome profile. There is no npm installation or build step. The public key in the manifest keeps the unpacked extension ID stable across machines; it is not a secret or an account identifier.

See [development](../docs/development.md), [protocol](../docs/protocol.md) and [acceptance checks](../docs/switcher.md). Preview capture is optional and excludes incognito, non-normal windows and internal browser pages.

From the repository root:

```powershell
node --test tests/extension.test.mjs tests/background.test.mjs tests/previews.test.mjs
```
