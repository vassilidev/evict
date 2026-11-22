# How Evict works

<sub>[README](../README.md) · [Features](features.md) · [Install](install.md) · [How it works](internals.md) · [Code signing](code-signing.md) · [Website](https://vassilidev.github.io/evict/)</sub>

## Why it's fast

### macOS

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
9. **System folders are walked once**: the sealed system volume (`/System`, `/usr`) only changes with a macOS update, so its folders' sizes are kept and each one shows as a single block: `/System` goes from 5.7 s to 1.4 s. The sizes are filled in while macOS asks for disk access, and dropped as soon as the macOS version changes.

### Linux

The same `scan.c`, with the Linux system calls:

1. **`getdents64(2)`** with a 128 KB buffer: a whole batch of entries per call, and `d_type` tells folders apart without any `stat()`. Only files get one `fstatat()`, for their allocated size (`st_blocks`).
2. **`openat(parent, name)`**, a **local stack per thread** and **8 threads**, as on macOS.
3. **Real disks only**: ext4, btrfs, xfs, f2fs, zfs… are read; `/proc`, `/sys`, tmpfs, overlay, network shares, FUSE, snap images and removable media (`/media`, `/run/media`) are skipped. A filesystem mounted twice (bind mounts) is read once, and the disk size adds up every disk read, so a separate `/home` partition counts.
4. **Real size on disk**, folders included, hard links counted once: the total matches `du` (checked in CI on Ubuntu, and by hand on Fedora).
5. **`/usr` is walked once**: it belongs to the package manager, so its folders' sizes are kept until the package database changes. The installer fills them in.

Benchmarks against `gdu`, `dua` and `ncdu` on Linux are to come.

### Windows

The same `scan.c`, with the NT system calls:

1. **`NtQueryDirectoryFile`** with a 128 KB buffer (`FileIdFullDirectoryInformation`): the name, attributes, allocated size and file id of a whole batch of entries per call. Files are never opened.
2. **`NtCreateFile` relative to the parent folder's handle**, the Windows `openat`, with a **local stack per thread** and **8 threads**.
3. **Every internal drive**: fixed drives (NTFS, ReFS, exFAT, FAT32) are read, removable and network drives aren't. Junctions and symbolic links aren't followed, so nothing is counted twice. OneDrive folders are read, online-only files count for what they take on disk (nothing), and nothing is downloaded.
4. **Real size on disk** (allocated size), hard links counted once on NTFS by file id, so `WinSxS` doesn't count the system twice.
5. **As administrator**, the backup privilege reads every folder whatever its permissions.

On GitHub's Windows runners (4 vCPUs), the app reads 1.4 million items in 6.4 s once the disk is in cache. Cold, the scan waits on the runner's throttled cloud disk (1.8 million items in about 8 minutes on Windows Server 2022). Benchmarks on a real PC against WizTree and WinDirStat are to come.

## Layout

```
scan.c            scanner (C, no dependencies, macOS, Linux and Windows)  →  JSON of the pruned tree
index.html        interface (plain HTML/CSS/JS), shared by the apps and the CLI  ←  data injected into the page
macos/            macOS app (SwiftUI + WKWebView, native bridge, self-updater; Info.plist holds the version),
                  DMG window background (dmg-background.html, rendered to .tiff), smoke.sh (DMG install test)
linux/            Linux app (evict.c: GTK 3 + WebKitGTK 4.1 or 4.0, same bridge), build.sh, package.sh (Docker build of the
                  release files), smoke.sh (install, start, uninstall test), desktop entry, polkit policy, nfpm packaging
windows/          Windows app (evict.c: Win32 + WebView2, same bridge), build.sh (llvm-mingw cross-build in Docker),
                  installer.nsi (one installer for x64, ARM64 and x86), smoke.ps1 (install, scan, start, uninstall test)
evict             CLI script: scan + page + open in the browser
build.sh          builds a universal Evict.app (Apple Silicon + Intel); "./build.sh dmg" makes the DMG
minify.py         minifies the page shipped in the apps and the website at deploy time (esbuild)
install.sh        builds and installs the app from source (macOS or Linux)
test.sh           checks the scanner's total against `du` on a test folder
test-page.js      checks the page's path rules (what can be deleted, categories) on macOS, Linux and Windows paths
docs/             website (GitHub Pages): home, download page, Linux and Windows installers (install.sh, install.ps1), images
```

Every push runs the [CI](../.github/workflows/ci.yml): the scanner test on macOS and Linux; the universal macOS build and DMG, then the app installed from the DMG, started, quit and removed ([macos/smoke.sh](../macos/smoke.sh)); the Linux build in an Ubuntu 22.04 container (the oldest glibc supported) for x86_64 and ARM64, with the `.deb`, `.rpm`, Arch package and tarball; then [on every supported distribution](../.github/workflows/linux-distros.yml), x86_64 and ARM64, each in a fresh container, the [one-line installer](install.sh) installs the package, Evict starts on a virtual display, scans and shows its map, and is uninstalled with nothing left behind ([linux/smoke.sh](../linux/smoke.sh)); the Windows build, cross-compiled for x64, ARM64 and x86, is [installed, scanned, started and uninstalled](../.github/workflows/windows.yml) on Windows Server 2022 and 2025 and Windows 11 ARM64, with a screenshot of the map ([windows/smoke.ps1](../windows/smoke.ps1)). Pushing a `v*` tag runs the [release workflow](../.github/workflows/release.yml): the Linux packages and the Windows installer are published as soon as they're built and tested, and the same checks run again on the published release, through the website's installers, as users get it; the release becomes "latest" once Apple has notarized the DMG, and when Apple takes longer than the release, [a scheduled workflow](../.github/workflows/notarized.yml) publishes that same DMG as soon as Apple accepts it. The [website](../.github/workflows/pages.yml) is deployed minified from `docs/` on every change.

## Porting to a new platform

The scanner, the page and its JSON are shared; a platform app is a window with a web view, the scanner, and a native bridge. The page talks to it with `window.webkit.messageHandlers.evict.postMessage({action, …})` (WKWebView on macOS, WebKitGTK on Linux; on Windows, a script injected at document start provides the same object and forwards to WebView2's `chrome.webview.postMessage`).

| Page → app | Fields | The app… |
| --- | --- | --- |
| `copy` | `text` | puts it on the clipboard |
| `reveal` | `path` or `paths` | shows the items in the file manager |
| `open` / `preview` | `path` / `paths` | opens with the default app / shows a quick preview |
| `delete` / `deleteMany` | `path`, `size` / `items: [[path, size]…]` | confirms (secrets listed), trashes or deletes, offers admin rights on failure |
| `emptyTrash` | `path`, `size` | empties the user's Trash, only that exact folder |
| `archive` | `id`, `path` | lists the archive's files |
| `dupes` | `id`, `groups: [[path…]…]` | compares candidates by sampled then full content |
| `ask` / `cancel` | `id`, `prompt` | streams Claude Code's answer / stops the running job |
| `menu` | `id` | runs a command from the page's ☰ menu (Windows: `rescan`, `admin`, `updates`, `update`) |
| `ready` | | the map is shown: the app puts its update button back |

| App → page | Called with |
| --- | --- |
| `evictLive(progress)` | `{count, bytes}` from the scanner's progress lines, on the scan screen |
| `evictRemoved(paths, freed)` | what was deleted (`true`) or moved to the Trash (`false`) |
| `evictEmptied(path)` | the Trash was emptied |
| `evictAnswer(id, text, kind)` | `kind` is `delta`, `done` or `error` |
| `evictArchive(id, result)` | `{count, total, entries: [[name, size]…]}` or `{error}` |
| `evictDupes(id, result)` | `{sets: [[[path, privateSize]…]…]}`, `-1` when the private size is unknown |
| `evictUpdate(text)` | shows the toolbar's update button with that label (Windows) |

Before the page loads, the app sets `window.evictClaude` (Claude Code found) and `window.evictLang`; the Windows app also sets `window.evictMenu` (the ☰ menu's commands) and `window.evictVersion`. The scanner runs as `evict-scan [--progress] [--home DIR] [--out FILE] [folder]`: the JSON report on stdout (`os`, `total`, `free`, `count`, `secs`, `date`, `home`, `tree`, and `trash` on Windows), progress lines on stderr (with `--out`, the report goes to FILE and the messages to FILE.log, for a scanner started elevated). The app puts the report in place of `window.DATA = null; /* __DATA__ */` in the page. On Windows, report paths look like POSIX ones (`/C:/Users/me`, and `/` for every drive when there are several), so the page's path logic is the same everywhere; the page shows them as `C:\Users\me`, and the app converts them back.

```sh
