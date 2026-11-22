<p align="center">
  <img src="docs/icon.png" width="112" alt="">
</p>

<h1 align="center">Evict</h1>

<p align="center">
  <b>The bouncer for your disk.</b><br>
  Evict reads your whole Mac in about twenty seconds and shows where every gigabyte went:<br>
  a map, a list, the duplicates, the caches you can drop. Then it helps you clean up, safely.
</p>

<p align="center">
  <a href="https://vassilidev.github.io/evict/"><b>Website</b></a> ·
  <a href="https://github.com/vassilidev/evict/releases/latest/download/Evict.dmg"><b>Download</b></a> ·
  <a href="https://github.com/vassilidev/evict/releases">Releases</a>
</p>

<p align="center">
  <a href="https://github.com/vassilidev/evict/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/vassilidev/evict/actions/workflows/ci.yml/badge.svg"></a>
  <img alt="macOS 13+" src="https://img.shields.io/badge/macOS-13%2B-111?logo=apple">
  <img alt="C + SwiftUI" src="https://img.shields.io/badge/C%20%2B%20SwiftUI-native-e0473d">
  <img alt="Zero dependencies" src="https://img.shields.io/badge/dependencies-0-2ea36b">
  <img alt="100% local scan" src="https://img.shields.io/badge/scan-100%25%20local-3d7fe6">
  <img alt="MIT license" src="https://img.shields.io/badge/license-MIT-8b93a1">
</p>

![Evict's home page: the disk by kind of file, cleanup shortcuts, where the space is, the largest files and the regenerable folders](docs/home.png)

<table>
  <tr>
    <td width="33%"><img src="docs/screenshot.png" alt="The map: every folder as a box sized by the space it takes"></td>
    <td width="33%"><img src="docs/search.png" alt="Search: video files as cards with their size, some selected"></td>
    <td width="33%"><img src="docs/duplicates.png" alt="Duplicates: sets of identical files with the copy to keep"></td>
  </tr>
  <tr>
    <td align="center"><sub>The map</sub></td>
    <td align="center"><sub>Search (<code>⌘K</code>)</sub></td>
    <td align="center"><sub>Duplicates</sub></td>
  </tr>
</table>
<p align="center"><sub>Screenshots taken on a fictional disk.</sub></p>

## What Evict does

### See where the space went

- **A home page**: the disk as one bar split by kind of file, tiles for the Trash, Downloads, regenerable files, duplicates and Claude's quick wins, then where the space is, the largest files and the regenerable folders. Each scan opens on it.
- **A map**, finviz-style: each box is as big as the space it takes. Click to go inside, use the path bar (with the folder's size) to go back up.
- **A list**: the same folder as a tree sorted by size, each row drawn over a bar of its size and color, folders expanded in place.
- **Colors that tell you something**: 🟥 regenerable (caches, `node_modules`, DerivedData, builds, `.venv`…), 🟧 archives and disk images, 🟪 media, 🟦 apps, 🟩 data, ⬛ system. Click a category to highlight it.
- **Dependency folders count as one block**: `node_modules`, `vendor`, `.venv`, `Pods`, `site-packages`… are added up without keeping the thousands of folders inside, which halves the report on a developer's disk.

### Find anything in a keystroke

- **Search** (`⌘K`, `⌘F` or `/`): a palette over the blurred map. With nothing typed it lists the largest files of the disk; otherwise every file and folder whose name matches, biggest first, as cards with their size, location and a size gauge. `↑` `↓` to move, `↵` to jump to the item, `⌘↵` to show it in Finder, `⌘Y` to preview it.
- **Filter by extension**: type `.mov .mp4`, or click one of the extensions that weigh the most, right under the search field.
- **Preview anything**: `Space` on a file, like in the Finder. Quick Look opens photos, videos, PDFs, Office and iWork documents, text, CSV, code and audio. **Show contents** lists what's inside zip, rar, 7z, tar.gz, iso and other archives, biggest files first. **Open with default app** opens the file.

### Clean up without regrets

- **Duplicates, compared by content**: files of the same size and type are checked by their start, middle and end, by kind (videos, photos, audio, archives, documents). Each set keeps one copy, and **Select extra copies** picks the others. APFS clones, which share their space with the original, are flagged: deleting them frees nothing.
- **Select many, delete once**: checkboxes in lists and search results, `⌘`-click on the map, `Shift`-click for a range, **Select all** on a search. The selection stays while you search and browse; a bar shows its total size, lets you review it, preview it, show it in Finder or delete it all at once.
- **Trash and Downloads one click away**: shortcuts show their size, items you move to the Trash from Evict show up there, and **Empty Trash…** frees the space for good.
- **Built-in cleanup** (app): move to Trash or delete permanently. The administrator password is only asked for when needed, once for a whole selection.
- **✦ Quick wins**: a personal cleanup plan for the whole disk. Evict sends Claude a digest of the scan (the biggest folders, regenerable folders, large files) and gets back the safest, biggest gains first, with each app's own cleanup command, what needs your judgment and what to leave alone.
- **Ask Claude** what a folder or file is: what it is, which app creates it, whether it's safe to delete (✅ / ⚠️ / ⛔) and how to clean it properly.

  In the app, answers come from your local [Claude Code](https://claude.com/claude-code) and stream into a side panel, formatted, with every path clickable. Claude Code runs Opus at low effort (a quick wins plan in about 20 seconds) and without any tools, so it can only answer: nothing is ever deleted for you. Without it, or in the browser, the prompt is copied, ready to paste into any AI chat.

### Safe and private

- **Careful with secrets**: `~/.ssh`, `~/.gnupg`, `~/.aws`, `~/.kube` and the keychains can't be deleted. Before any deletion, Evict looks inside for `.env` files, private keys (`.pem`, `.key`, `.p12`, `id_rsa`…) and credentials, and lists them in the confirmation: they're often in no git repository or backup.
- **Protected paths**: the disk root, top-level and home folders, `~/Library` and macOS system folders are never offered for deletion.
- **100% local**: the scan never leaves your Mac. No telemetry, no dependencies, just a C scanner and an HTML page. The only network requests are the update check against GitHub and, when you click **Ask Claude** or **Quick wins**, that prompt (names and sizes from the scan, never file contents).
- **Hide a folder** (`System`, for example) to give the rest more room. The choice is remembered across scans.

### Fast

- **The whole disk in about 20 s**: 5.7 million files (426 GB) on a Mac mini M4 Pro, 27 s cold, right after a reboot.
- **A light scan screen**: blocks build up while the disk is read, with the real item count, volume, elapsed time and items per second. The animation only moves layers on the GPU, so the scan keeps its full speed.
- **Built-in updates**: Evict checks GitHub for new versions at launch and every 6 hours, tells you when one is out, and installs it in one click.

## Install

### Download

**[⬇ Download Evict.dmg](https://github.com/vassilidev/evict/releases/latest/download/Evict.dmg)** (macOS 13+, Apple Silicon and Intel). Open the DMG, then drag Evict into Applications.

Evict is signed with a Developer ID and notarized by Apple, so it opens like any other app.

> **On first launch**, Evict asks for **Full Disk Access**. Without it, macOS interrupts the scan with one permission prompt per protected folder (Desktop, Documents, other apps' data…). Click **Open System Settings**, turn Evict on, and the scan starts by itself. **Scan Anyway** is still available.

### Updates

Updates come from [GitHub Releases](https://github.com/vassilidev/evict/releases), not from git: each release ships the signed DMG built by CI. Evict checks for a new version at launch and every 6 hours. When one is out, it tells you once, then keeps an **Update** button in the toolbar. **Install and Relaunch** downloads the DMG, checks it really contains Evict, swaps the app in place and reopens it. You can also use **Evict → Check for Updates…** in the menu bar, or update from a terminal:

```sh
/Applications/Evict.app/Contents/MacOS/Evict --update
```

### Build from source

Requirements: macOS 13 or later, and Apple's command line tools (`xcode-select --install`).

```sh
git clone https://github.com/vassilidev/evict.git
cd evict
./install.sh
```

`install.sh` builds Evict.app, installs it in `/Applications` (or `~/Applications`), then opens it. After that, launch Evict from Spotlight whenever you like, and press **⌘R** to rescan. To update a build from source, `git pull && ./install.sh`; the in-app updater also works and replaces it with the latest release.

### Command line

```sh
./evict              # the whole disk, report opened in your browser
./evict ~/Projects   # a single folder
```

In the browser, a web page can't delete files, drive Finder or run Claude Code, so the menu copies text instead: **Copy rm -rf command**, an `open -R` command for **Show in Finder**, `qlmanage -p` for **Preview**, `tar -tvf` for **Show contents**, and the prompt for **Copy AI prompt**.

## Usage

| Action | Result |
| --- | --- |
| Click a folder | Drill into it (map) · expand it in place (list) |
| Double-click a folder (list) | Drill into it |
| Click a file · right-click · `⋯` | Menu: open, show in Finder, copy path, ask AI, hide, delete |
| **Home** / **Map** / **List** | Switch views (each scan opens on Home) |
| `⌘K`, `⌘F` or `/` | Search palette · `Esc` closes |
| Checkbox · `⌘`-click · `Shift`-click · `⌘A` | Select · add to the selection · select a range · select every row shown |
| `Esc` with a selection | Clear the selection |
| `Space` | Preview the item under the mouse, or the selection (Quick Look); archives show their contents |
| `.mov .mp4` in the search palette | Filter by extension |
| **Trash** / **Downloads** | Open them · **Empty Trash…** when you're in the Trash |
| **⋯** next to the path | Current folder: show in Finder, copy path, ask Claude, delete |
| **✦ Quick wins** | A personal cleanup plan for the whole disk |
| Click a path in Claude's answer | Jump to it in the map |
| `⌫` or `Esc` | Go up one level |
| Click a category | Highlight it |
| "N hidden" button | Show hidden folders again |
| `⌘R` (app) | Rescan |

## Why it's fast

Measured on the same Mac (Apple M4 Pro, internal SSD, 5.7 M entries):

| Method | Time |
| --- | ---: |
| Python, `os.scandir` + `lstat`, 1 thread | 153 s |
| First C version: full paths, global work queue, 36 threads | 41 s |
| **Evict**: `getattrlistbulk` + `openat`, 8 threads | **20 s** (27 s cold) |

1. **`getattrlistbulk(2)`**: one system call returns the name, type, allocated size and link count of hundreds of entries at once. There is no `stat()` per file.
2. **`openat(parent, name)`**: no full path is ever resolved, so threads don't fight over the kernel nodes of `/` and `/Users`.
3. **A local stack per thread** (depth-first): work is only shared when a thread is idle.
4. **8 threads, measured**: beyond that, APFS saturates on its internal locks. At 24 threads, kernel time climbs to 309 s cumulated while the scan gets *slower*.
5. **No iCloud downloads**: with `IOPOL_MATERIALIZE_DATALESS_FILES_OFF`, iCloud or Drive files that aren't downloaded are never fetched during the scan.
6. **Real size on disk** (`ATTR_FILE_ALLOCSIZE`): hard links are counted once, and files under 1 MB are grouped. A whole-disk report weighs about 3 MB.
7. **Nothing is counted twice**: the Data volume, reachable through firmlinks, and volumes from other containers are skipped.
8. **No endless wait**: if a folder stays blocked behind a macOS permission prompt, the scan stops after 30 s without progress and returns partial results, with a warning.

## Under the hood

```
scan.c        scanner (C, no dependencies)  →  JSON of the pruned tree
index.html    interface (plain HTML/CSS/JS) ←  data injected into the page
app/          macOS app (SwiftUI + WKWebView, JS ↔ native bridge, self-updater)
evict         CLI script: scan + page + open in the browser
build.sh      builds a universal Evict.app (Apple Silicon + Intel); "./build.sh dmg" makes the DMG
minify.py     minifies the page shipped in the app (esbuild), the repo keeps the readable source
install.sh    builds and installs Evict.app
test.sh       checks the scanner's total against `du` on a test folder
docs/         website (GitHub Pages) and README images
```

Every push runs the [CI](.github/workflows/ci.yml): scanner test, universal build, DMG. Pushing a `v*` tag runs the [release workflow](.github/workflows/release.yml), which builds the DMG and attaches it to the GitHub release. The app's built-in updater then picks it up.

```sh
# bump CFBundleShortVersionString in app/Info.plist, commit, then:
git tag v1.10.0 && git push origin v1.10.0
```

## Known limitations

- **macOS only for now**: [Linux](https://github.com/vassilidev/evict/issues/1) and [Windows](https://github.com/vassilidev/evict/issues/2) are planned.
- **APFS clones**: they are counted at full size, so the total can exceed the space actually used.
- **Snapshots**: APFS and Time Machine snapshots aren't visible. They show up in the "Invisible" box.
- **Search** only finds items of 1 MB or more: the scanner groups smaller files into "other items" to keep the report light.

## Roadmap

- [ ] [Native Linux support](https://github.com/vassilidev/evict/issues/1): `getdents64` + batched `statx` through io_uring, WebKitGTK
- [ ] [Native Windows support](https://github.com/vassilidev/evict/issues/2): direct NTFS MFT reading, WebView2

## License

[MIT](LICENSE).
