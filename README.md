<p align="center">
  <img src="docs/icon.png" width="112" alt="">
</p>

<h1 align="center">Evict</h1>

<p align="center">
  <b>The bouncer for your disk.</b><br>
  See where every gigabyte went in seconds, then clean up safely.<br>
  Free, open source and native on macOS, Windows and Linux.
</p>

<p align="center">
  <a href="https://vassilidev.github.io/evict/"><b>Website</b></a> ·
  <a href="https://vassilidev.github.io/evict/download.html"><b>Download</b></a> ·
  <a href="docs/features.md">Features</a> ·
  <a href="docs/install.md">Install guide</a> ·
  <a href="https://github.com/vassilidev/evict/releases">Releases</a>
</p>

<p align="center">
  <a href="https://github.com/vassilidev/evict/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/vassilidev/evict/actions/workflows/ci.yml/badge.svg"></a>
  <img alt="macOS 11+" src="https://img.shields.io/badge/macOS-11%2B-111?logo=apple">
  <img alt="Windows 10 and 11, x64, ARM64 and x86" src="https://img.shields.io/badge/Windows-10%20%7C%2011-111?logo=windows&logoColor=white">
  <img alt="Linux x86_64 and ARM64" src="https://img.shields.io/badge/Linux-x86__64%20%7C%20ARM64-111?logo=linux&logoColor=white">
  <img alt="MIT license" src="https://img.shields.io/badge/license-MIT-8b93a1">
</p>

![Evict's map: every folder of the disk as a box sized by the space it takes, colored by kind of file](docs/screenshot.png)

<table>
  <tr>
    <td width="33%"><img src="docs/list.png" alt="The list: folders sorted by size, each row drawn over a bar of its size"></td>
    <td width="33%"><img src="docs/search.png" alt="Search: video files as cards with their size, some selected"></td>
    <td width="33%"><img src="docs/duplicates.png" alt="Duplicates: sets of identical files with the copy to keep"></td>
  </tr>
  <tr>
    <td align="center"><sub>The list</sub></td>
    <td align="center"><sub>Search (<code>⌘K</code> or <code>Ctrl K</code>)</sub></td>
    <td align="center"><sub>Duplicates</sub></td>
  </tr>
</table>
<p align="center"><sub>Screenshots taken on a fictional disk.</sub></p>

<p align="center">
  <a href="#why-evict">Why Evict</a> ·
  <a href="#install">Install</a> ·
  <a href="#questions">Questions</a> ·
  <a href="#documentation">Documentation</a> ·
  <a href="#roadmap">Roadmap</a> ·
  <a href="#contributing">Contributing</a>
</p>

## Why Evict

Your disk is full and nothing tells you why. Evict reads all of it in seconds and draws it as a map where the biggest things are the biggest boxes. Then it helps you get the space back without breaking anything.

- **The whole disk at a glance.** A treemap of every folder, or the same folder as a list sorted by size. Colors tell you what each thing is: caches and builds that regenerate on their own, archives and disk images, media, apps, your data, the system.
- **Find anything in a keystroke.** `⌘K` (`Ctrl K` on Windows and Linux) searches the whole disk, largest files first. Type `.mov .iso` to filter by extension, press `Space` to preview, open archives without extracting them.
- **It knows a developer's disk.** `node_modules`, `.venv`, `vendor`, `Pods`, DerivedData and build folders show up as one block each, marked as regenerable.
- **Duplicates, for real.** Files are compared by their content, and one click selects every extra copy.
- **Clean up without regrets.** Everything goes to the Trash or Recycle Bin first. System folders, home folders and the places your keys live can't be deleted, and Evict lists the `.env` files and private keys inside a folder before it goes.
- **Ask before you delete.** Optional answers from your own [Claude Code](https://claude.com/claude-code): what a folder is, whether it's safe to remove, or a cleanup plan for the whole disk. Claude only sees names and sizes, and can't delete anything.
- **Fast.** 5.7 million files in about 20 seconds on a Mac. 1.4 million in 6.4 seconds on Windows once the disk is in cache.
- **Private and native.** A C scanner and one page in a native window, with no account and no telemetry. The only network request is the update check.

## Install

**macOS** 11 or later, Apple Silicon and Intel: **[⬇ Download Evict.dmg](https://github.com/vassilidev/evict/releases/latest/download/Evict.dmg)**. Signed and notarized by Apple. Drag Evict into Applications, then allow Full Disk Access when it asks.

**Windows** 10 and 11, on x64, ARM64 and 32-bit PCs: **[⬇ Download Evict-Setup.exe](https://github.com/vassilidev/evict/releases/latest/download/Evict-Setup.exe)**. It installs for your account, no administrator rights needed. Or from PowerShell:

```powershell
irm https://vassilidev.github.io/evict/install.ps1 | iex
```

**Linux** x86_64 and ARM64, on Ubuntu, Debian, Mint, Fedora, RHEL, openSUSE, Arch and more: one line installs the native package with its dependencies.

```sh
curl -fsSL https://vassilidev.github.io/evict/install.sh | sh
```

Evict updates itself. Checksums, manual packages, requirements and uninstalling are in the **[install guide](docs/install.md)** and on the **[download page](https://vassilidev.github.io/evict/download.html)**.

## Questions

**Is it really free?** Yes. MIT license, no account, no ads, no telemetry, no paid tier.

**Can Evict delete something by mistake?** It never deletes on its own: every deletion goes through a confirmation that lists what will go, with the Trash as the default. System folders, your home folder and where keys and passwords live can't be deleted at all. [What's protected →](docs/features.md#safe-and-private)

**Does it send my files anywhere?** No. The scan stays on your computer. **Ask Claude** is optional and sends names and sizes from the scan, never file contents. [Privacy details →](docs/features.md#safe-and-private)

**Which systems does it run on?** macOS 11 Big Sur or later; Windows 10 and 11 on x64, ARM64 and 32-bit PCs; Linux with glibc 2.31 or later on x86_64 and ARM64, tested on more than 20 distributions. [Requirements →](docs/install.md)

**Why does Windows warn about the installer?** It isn't code-signed yet, so SmartScreen doesn't know it: click **More info**, then **Run anyway**. The PowerShell line doesn't trigger the warning and checks the download's SHA-256. [Windows install →](docs/install.md#windows)

**Why does macOS ask for Full Disk Access?** Without it, macOS hides part of the disk and interrupts the scan with a prompt per protected folder. [More →](docs/install.md#questions)

**Why is part of my disk "not visible to the scan"?** Some folders need administrator rights, and snapshots are never visible. **Rescan as Administrator** reads the rest on Windows and Linux. [More →](docs/features.md#questions)

**How do I update or uninstall it?** Evict checks for updates on its own and installs them in one click on macOS and Windows. To uninstall, drag it to the Trash on macOS, use **Settings → Apps** on Windows, or add `--uninstall` to the install line on Linux. [Updates and uninstalling →](docs/install.md#updates)

**Can it scan an external drive or a single folder?** The app reads every internal disk. From a copy of the repository, `./evict /path/to/drive` scans any folder on macOS and Linux and opens the report in your browser. [Command line →](docs/install.md#command-line)

**How can it be that fast?** It asks the system for hundreds of files per call, never builds a full path, and walks the disk on eight threads. [How it works →](docs/internals.md)

**How does it compare to WinDirStat, WizTree, DaisyDisk or Baobab?** Same family: a treemap of your disk. Evict adds search, duplicates by content, regenerable folders flagged, protected paths and secrets, and the same app on all three systems, for free. [All features →](docs/features.md)

**Something's wrong, or missing?** [Open an issue](https://github.com/vassilidev/evict/issues): bug reports and ideas are welcome.

## Documentation

| Guide | What's inside |
| --- | --- |
| **[Features](docs/features.md)** | Everything Evict does, keyboard shortcuts, what's protected, known limitations |
| **[Install, update and build](docs/install.md)** | Every platform and its requirements, manual packages, updates, uninstalling, building from source, the command line |
| **[How it works](docs/internals.md)** | Why the scanner is fast on each system, the project layout, the bridge between the page and the apps, CI and releases |
| **[Website](https://vassilidev.github.io/evict/)** | The tour, the speed race and the download page with every file's checksum |

## Roadmap

- [x] Native [Linux](https://github.com/vassilidev/evict/issues/1) and [Windows](https://github.com/vassilidev/evict/issues/2) apps, with one-line installers
- [x] Older systems: macOS 11, Windows on ARM64 and 32-bit PCs, RHEL 9, Debian 11 and Ubuntu 20.04
- [ ] Faster administrator scans on Windows by reading the NTFS MFT directly, like WizTree
- [ ] A code-signed Windows installer, and a winget package
- [ ] Benchmarks on real PCs against WizTree, WinDirStat, `gdu`, `dua` and `ncdu`

## Contributing

Issues and pull requests are welcome. Evict builds from source in a minute on macOS and Linux, and in a container for Windows: see [building from source](docs/install.md#build-from-source). Every pull request is built and tested on macOS, Windows and more than 20 Linux distributions; [how it works](docs/internals.md) explains the layout and the bridge every app implements.

## License

[MIT](LICENSE).
