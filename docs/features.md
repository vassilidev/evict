# Features

<sub>[README](../README.md) · [Features](features.md) · [Install](install.md) · [How it works](internals.md) · [Code signing](code-signing.md) · [Website](https://vassilidev.github.io/evict/)</sub>

Everything Evict does, on macOS, Windows and Linux.

## See where the space went

- **A map**, finviz-style: each box is as big as the space it takes. Click to go inside, use the path bar (with the folder's size) to go back up, click the logo to see the whole disk again.
- **A list**: the same folder as a tree sorted by size, each row drawn over a bar of its size and color, folders expanded in place.
- **Colors that tell you something**: 🟥 regenerable (caches, `node_modules`, DerivedData, builds, `.venv`…), 🟧 archives and disk images, 🟪 media, 🟦 apps, 🟩 data, ⬛ system. Click a category to highlight it.
- **Free space always in view**: a small gauge in the toolbar shows what's left on the disk, green while there's room, orange then red when it runs out, and it grows as you clean up.
- **Dependency folders count as one block**: `node_modules`, `vendor`, `.venv`, `Pods`, `site-packages`… are added up without keeping the thousands of folders inside, which halves the report on a developer's disk.

## Find anything in a keystroke

- **Search** (`⌘K`, `⌘F` or `/`; `Ctrl` instead of `⌘` on Windows and Linux): a palette over the blurred map. With nothing typed it lists the largest files of the disk; otherwise every file and folder whose name matches, biggest first, as cards with their size, location and a size gauge. `↑` `↓` to move, `↵` to jump to the item, `⌘↵` to show it in Finder (Explorer on Windows, the file manager on Linux), `⌘Y` to preview it.
- **Filter by extension**: type `.mov .mp4`, or click one of the extensions that weigh the most, right under the search field.
- **Preview anything**: `Space` on a file, like in the Finder. On macOS, Quick Look opens photos, videos, PDFs, Office and iWork documents, text, CSV, code and audio; on Windows, [QuickLook](https://github.com/QL-Win/QuickLook) does when it's installed, and on Linux GNOME Sushi, otherwise the default app opens the file. **Show contents** lists what's inside zip, rar, 7z, tar.gz, iso and other archives, biggest files first (on Windows through the `tar` built into Windows 10 and 11, on Linux through `bsdtar` from libarchive when installed, GNU `tar` otherwise). **Open with default app** opens the file.

## Clean up without regrets

- **Duplicates, compared by content**: files of the same size and type are checked by their start, middle and end, by kind (videos, photos, audio, archives, documents). Each set keeps one copy, and **Select extra copies** picks the others. On macOS, APFS clones, which share their space with the original, are flagged: deleting them frees nothing.
- **Select many, delete once**: checkboxes in lists and search results, `⌘`-click on the map, `Shift`-click for a range, **Select all** on a search. The selection stays while you search and browse; a bar shows its total size, lets you review it, preview it, show it in Finder or delete it all at once.
- **Trash and Downloads one click away**: shortcuts show their size (`~/.Trash` on macOS, the Recycle Bin on Windows, `~/.local/share/Trash` on Linux), items you move to the Trash from Evict show up there (on Windows after a rescan, since the Recycle Bin renames them), and **Empty Trash…** frees the space for good.
- **Built-in cleanup** (app): move to the Trash or Recycle Bin, or delete permanently. The administrator password is only asked for when needed, once for a whole selection (UAC on Windows, `pkexec` and the desktop's polkit prompt on Linux).
- **✦ Quick wins**: a personal cleanup plan for the whole disk. Evict sends Claude a digest of the scan (the biggest folders, regenerable folders, large files) and gets back the safest, biggest gains first, with each app's own cleanup command, what needs your judgment and what to leave alone.
- **Ask Claude** what a folder or file is: what it is, which app creates it, whether it's safe to delete (✅ / ⚠️ / ⛔) and how to clean it properly.

  In the app (macOS, Windows and Linux), answers come from your local [Claude Code](https://claude.com/claude-code) and stream into a side panel, formatted, with every path clickable. Claude Code runs Opus at low effort (a quick wins plan in about 20 seconds) and without any tools, so it can only answer: nothing is ever deleted for you. Without it, or in the browser, the prompt is copied, ready to paste into any AI chat.

## Safe and private

- **Careful with secrets**: `~/.ssh`, `~/.gnupg`, `~/.aws`, `~/.kube`, the macOS keychains, the Windows credentials and keys (`AppData\Roaming\Microsoft\Protect`, `Credentials`, `Vault`…) and the Linux keyrings can't be deleted. Before any deletion, Evict looks inside for `.env` files, private keys (`.pem`, `.key`, `.p12`, `id_rsa`…) and credentials, and lists them in the confirmation: they're often in no git repository or backup.
- **Protected paths**: the disk root, top-level and home folders, system folders (`/System`, `/usr`, `/etc`, `/boot`, `/proc`…, `C:\Windows`, `System Volume Information`, `pagefile.sys`…), `~/Library` on macOS, `AppData` on Windows and `~/.config`, `~/.local` on Linux are never offered for deletion.
- **100% local**: the scan never leaves your computer. No telemetry, no dependencies, just a C scanner and an HTML page. The only network requests are the update check against GitHub and, when you click **Ask Claude** or **Quick wins**, that prompt (names and sizes from the scan, never file contents).
- **Hide a folder** (`System`, for example) to give the rest more room. The choice is remembered across scans.

## Fast

- **The whole disk in about 20 s on macOS**: 5.7 million files (426 GB) on a Mac mini M4 Pro, 27 s cold, right after a reboot. On Windows, 1.4 million items in 6.4 s on a CI runner once the disk is in cache. Linux benchmarks are to come.
- **System folders read once**: on macOS and Linux, the parts of the system that only change with an update (the macOS system volume, `/usr` on Linux) are walked once and remembered until the next update.
- **A light scan screen**: blocks build up while the disk is read, with the real item count, volume, elapsed time and items per second. The animation only moves layers on the GPU, so the scan keeps its full speed.
- **Built-in updates**: Evict checks GitHub for new versions at launch and every 6 hours and tells you when one is out. On macOS and Windows it installs it in one click; on Linux, the install command updates it.

## Shortcuts

`⌘` on macOS is `Ctrl` on Windows and Linux.

| Action | Result |
| --- | --- |
| Click a folder | Drill into it (map) · expand it in place (list) |
| Double-click a folder (list) | Drill into it |
| Click a file · right-click · `⋯` | Menu: open, show in Finder (Explorer on Windows, folder on Linux), copy path, ask AI, hide, delete |
| **Map** / **List** | Switch views (the map always opens first) |
| Click the logo | Back to the whole disk, on the map |
| `⌘K`, `⌘F` or `/` | Search palette · `Esc` closes |
| Checkbox · `⌘`-click · `Shift`-click · `⌘A` | Select · add to the selection · select a range · select every row shown |
| `Esc` with a selection | Clear the selection |
| `Space` | Preview the item under the mouse, or the selection (Quick Look on macOS, QuickLook on Windows); archives show their contents |
| `.mov .mp4` in the search palette | Filter by extension |
| **Trash** / **Downloads** | Open them · **Empty Trash…** when you're in the Trash |
| **⋯** next to the path | Current folder: show in Finder, copy path, ask Claude, delete |
| **✦ Quick wins** | A personal cleanup plan for the whole disk |
| Click a path in Claude's answer | Jump to it in the map |
| `⌫` or `Esc` | Go up one level |
| Click a category | Highlight it |
| "N hidden" button | Show hidden folders again |
| `⌘R` (app) | Rescan (`F5` too on Windows) |
| **Rescan as Administrator** (Windows and Linux apps) | Read the folders a normal user can't |
| **☰** (Windows app) | Rescan, Rescan as Administrator, Check for Updates |

## Questions

**Can Evict delete something by mistake?** It never deletes on its own: everything goes through a confirmation that lists what will go, with the Trash (Recycle Bin) as the default. The disk root, your home folder, system folders, `~/Library` on macOS, `AppData` on Windows, `~/.config` on Linux, keychains, keyrings, Windows credentials and folders like `~/.ssh` can't be deleted, and Evict warns you about the `.env` files and keys inside a folder before it goes.

**Does Evict send my files anywhere?** No. The scan stays on your computer; the only request is the update check against GitHub. **Ask Claude** and **Quick wins** are optional and send names and sizes from the scan, never file contents, through your own Claude Code.

**Why is part of my disk "not visible to the scan"?** A normal account can't read some folders: Docker images, system logs and other users' homes on Linux; restore points, other users' profiles and some system folders on Windows. **Rescan as Administrator** reads them, after the polkit (Linux) or UAC (Windows) prompt. On macOS, Full Disk Access does the same. NTFS's own files (`$MFT`, the journal) and snapshots stay invisible to any scan that walks folders.

## Known limitations

- **Windows**: the installer isn't code-signed yet, so SmartScreen warns about a downloaded copy; previews need [QuickLook](https://github.com/QL-Win/QuickLook), otherwise the default app opens the file; items moved to the Recycle Bin show up there after a rescan; administrator scans walk folders like normal ones instead of reading the NTFS MFT directly, so a cold scan waits on the disk; Windows 7 and 8.1 aren't supported.
- **Shared blocks**: APFS clones, reflinks or snapshots on Btrfs and ZFS, and block clones on ReFS (Dev Drive) are counted at full size, so the total can exceed the space actually used.
- **Snapshots**: APFS and Time Machine snapshots, and Windows restore points, aren't visible. They show up in the "Invisible" box, with NTFS's own files (`$MFT`, the journal).
- **Search** only finds items of 1 MB or more: the scanner groups smaller files into "other items" to keep the report light.
- **Linux**: no benchmarks yet; musl distributions (Alpine) aren't supported; updates go through the install command, not a one-click update; previews need GNOME Sushi, otherwise the default app opens the file; archives other than tar need `bsdtar` (libarchive).
