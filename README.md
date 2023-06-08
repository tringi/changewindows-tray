# ChangeWindows.org Notification

Unofficial Tray Notification Application for https://changewindows.org project.
Designed for smallest possible footprint.

## Configuring pre-Alpha version

1. Run `ChangeWindows.exe` and wait for it to download all info (tray icon tooltip stops showing *"Checking..."*).
2. Open `regedit.exe` and go to: `HKEY_CURRENT_USER\SOFTWARE\TRIM CORE SOFTWARE s.r.o.\ChangeWindows`
3. Scroll through `builds` to see how builds and updates are named
4. Go to `alerts` subkey.
4. Add values (type is irelevant) where name is filename wildcards -style mask for the builds. Those will be reported by alert.

**Example**
* `*` - report everything
* `PC;*` - report all PC updates
* `PC;Canary` - report new builds in Canary channel for PC
* `PC;*#22???` - report all build 22000 to 22999 updates for PCs in any channel
* `*#17763` - report everything regarding build 17763 regardless platform or channel

## Libraries used:
* https://github.com/vivkin/gason
