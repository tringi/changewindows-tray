# ChangeWindows.org Notification

Unofficial Tray Notification Application for https://changewindows.org project.  
Designed for smallest possible footprint.

## Usage

Download appropriate executable from the *builds* directory and run it.  
Optionally copy it into your Program Files and make a link in Startup to have it started automatically.

After the initial sync the Settings dialog will be available to configure reporting.

## Manual configuration

`HKEY_CURRENT_USER\SOFTWARE\TRIM CORE SOFTWARE s.r.o.\ChangeWindows\alerts`
* values' (type is irelevant) name is filename wildcards -style mask for the builds; those will be reported by alert

**Example**
* `*` - report everything
* `PC;*` - report all PC updates
* `PC;Canary` - report new builds in Canary channel for PC
* `PC;*#22???` - report all build 22000 to 22999 updates for PCs in any channel
* `*#17763` - report everything regarding build 17763 regardless platform or channel

## Libraries used:
* https://github.com/vivkin/gason
