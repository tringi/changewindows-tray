# ChangeWindows.org Notification

**Unofficial** Tray Notification Application for https://changewindows.org project.  
Designed for smallest possible footprint. By [TRIM CORE SOFTWARE](https://www.trimcore.cz) s.r.o.

## Installation

Download appropriate executable from the [builds directory](tree/main/builds),
copy it into your *Program Files*, and make a link in Startup to have it started automatically.

After the initial sync, a Settings dialog will be available to configure reporting.

## Settings

Use text inputs/dropdowns and + button to add combinations of releases that you wish to be notified about.  
Trash button deletes selected ones. Double-click existing entry to switch to edit mode.

Use DOS filename wildcards, i.e. `22???` for build will notify about builds 22000 to 22999, `*` means everything.
Leaving *Channel/Release* empty will report on new builds, not updates to existing, although this distinction
is mostly irrelevant.

Options:

* **Check every # minutes** - keep as large as reasonable to not DDoS the website
* **Animate notification icon** - currently cannot be disabled (off option not implemented)
* **Check for legacy platforms** - do not ignore platforms marked as *legacy*, e.g.: Mobile, IoT and 10X
* **Group toast by build numbers** - use *Build* notification format if possible (see below)

## Command-line parameters

When you run `ChangeWindows.exe param` the following happens depending on `param`:

* `hide` – starts the program without tray icon, or hides the existing one
* `show` – restores hidden tray icon of the running program (if any is running)
* `check` – instructs the running program to check for any updates
* `settings` – instructs the running instance to open Settings dialog
* `terminate` – closes the running instance

## Manual configuration

`HKEY_CURRENT_USER\SOFTWARE\TRIM CORE SOFTWARE s.r.o.\ChangeWindows\alerts`
* values' (type is irelevant) name is filename wildcards -style mask for the builds; those will be reported by alert

**Example**
* `*` - report everything
* `PC;*` - report all PC updates
* `PC;Canary` - report new builds in Canary channel for PC
* `PC;*#22???` - report all build 22000 to 22999 updates for PCs in any channel
* `*#17763` - report everything regarding build 17763 regardless platform or channel

## Toast notifications

TBD: description, mode selection

### Single platform

### Platforms

### Releases

Not implemented yet.

### Builds


## Libraries used:
* https://github.com/vivkin/gason
