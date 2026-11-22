# Install, update and build

<sub>[README](../README.md) · [Features](features.md) · [Install](install.md) · [How it works](internals.md) · [Code signing](code-signing.md) · [Website](https://vassilidev.github.io/evict/)</sub>

Every file, with its SHA-256 checksum, is on the **[download page](https://vassilidev.github.io/evict/download.html)**.

## macOS

**[⬇ Download Evict.dmg](https://github.com/vassilidev/evict/releases/latest/download/Evict.dmg)** (macOS 11 Big Sur or later, Apple Silicon and Intel). Open the DMG, then drag Evict into Applications.

Evict is signed with a Developer ID and notarized by Apple, so it opens like any other app.

> **On first launch**, Evict asks for **Full Disk Access**. Without it, macOS interrupts the scan with one permission prompt per protected folder (Desktop, Documents, other apps' data…). Click **Open System Settings**, turn Evict on, and the scan starts by itself. **Scan Anyway** is still available.

## Windows

**[⬇ Download Evict-Setup.exe](https://github.com/vassilidev/evict/releases/latest/download/Evict-Setup.exe)**: one installer for every PC on Windows 10 or 11, Intel and AMD (x64), ARM64 and 32-bit (x86). It installs Evict for your account, no administrator rights needed, with a Start menu entry; uninstall it from **Settings → Apps**.

Or from PowerShell, which also updates it:

```powershell
irm https://vassilidev.github.io/evict/install.ps1 | iex
```

The script downloads the installer from the latest release and checks its SHA-256 against the release before running it.

> **SmartScreen may warn you** the first time you open the downloaded installer: it isn't code-signed yet. Click **More info**, then **Run anyway**. The PowerShell line doesn't trigger the warning.

Evict draws its window with Microsoft Edge WebView2, which comes with Windows 11 and up-to-date Windows 10. Where it's missing, Evict offers to download and install it from Microsoft. CI installs, scans, starts and uninstalls Evict on Windows Server 2022 and 2025 (x64) and Windows 11 (ARM64), and checks the 32-bit scanner too. Windows 7 and 8.1 aren't supported: Microsoft stopped updating WebView2 for them in 2023.

**On Windows**, the app has the same interface, with `Ctrl` instead of `⌘`. It reads every internal drive (C:, D:…), and a single drive opens directly. **Show in Explorer** selects the items, deletion goes to the Recycle Bin, and **Rescan as Administrator** (☰ menu, top right) reads the folders a normal account can't, after the UAC prompt.

## Linux

One line, on any distribution:

```sh
curl -fsSL https://vassilidev.github.io/evict/install.sh | sh
```

The script picks the native package for your distribution: apt gets the `.deb`, dnf and zypper the `.rpm`, pacman the Arch package. The package manager installs the dependencies (it asks for your password) and Evict shows up in your app menu. On other glibc distributions, it unpacks the tarball into `~/.local`. Run the same line again to update. To uninstall:

```sh
curl -fsSL https://vassilidev.github.io/evict/install.sh | sh -s -- --uninstall
```

**Requirements**: x86_64 or ARM64, glibc 2.31 or later, WebKitGTK 4.1 or 4.0 (GTK 3): Evict loads whichever the system has. CI installs, starts and uninstalls Evict on Ubuntu 20.04, 22.04, 24.04 and 26.04, Debian 11, 12 and 13, Linux Mint 20, 21 and 22, Kali, Fedora 41 to 43, openSUSE Leap 15.6, Leap 16 and Tumbleweed, Arch, Manjaro, Rocky Linux and AlmaLinux 9 and 10, on x86_64 and ARM64; derivatives like Pop!_OS and EndeavourOS use the same packages (on Rocky and Alma 10, the installer enables EPEL for WebKitGTK). musl distributions like Alpine aren't supported.

**Manual install**: every release ships packages for both architectures, listed on the [download page](https://vassilidev.github.io/evict/download.html) with their checksums. Replace `x86_64` with `aarch64` on ARM.

| Distribution | File | Command |
| --- | --- | --- |
| Debian, Ubuntu, Mint, Pop!_OS | `evict-linux-x86_64.deb` | `sudo apt install ./evict-linux-x86_64.deb` |
| Fedora, Rocky, Alma, RHEL | `evict-linux-x86_64.rpm` | `sudo dnf install ./evict-linux-x86_64.rpm` |
| openSUSE | `evict-linux-x86_64.rpm` | `sudo zypper install --allow-unsigned-rpm ./evict-linux-x86_64.rpm` |
| Arch, Manjaro, EndeavourOS | `evict-linux-x86_64.pkg.tar.zst` | `sudo pacman -U ./evict-linux-x86_64.pkg.tar.zst` |
| Anything else (WebKitGTK 4.1 or 4.0 installed) | `evict-linux-x86_64.tar.gz` | extract it, then run `evict/evict` |

**On Linux**, the app has the same interface as on macOS, with `Ctrl` instead of `⌘`. **Show in folder** opens your file manager (Files, Dolphin, Nemo…) with the item selected, and deletion goes to the desktop's Trash. As a normal user, some folders can't be read (Docker images in `/var/lib/docker`, system logs, other users' homes): that space shows up as "not visible to the scan", and **Rescan as Administrator** reads it all after the polkit password prompt.

## Updates

Updates come from [GitHub Releases](https://github.com/vassilidev/evict/releases), not from git: each release ships the signed DMG, the Windows installer and the Linux packages built by CI. Evict checks for a new version at launch and every 6 hours. When one is out, it tells you once, then keeps an **Update** button in the toolbar.

- **macOS**: **Install and Relaunch** downloads the DMG, checks it really contains an Evict signed by its developer, swaps the app in place and reopens it. You can also use **Evict → Check for Updates…** in the menu bar, or update from a terminal:

  ```sh
  /Applications/Evict.app/Contents/MacOS/Evict --update
  ```

- **Windows**: **Install and Relaunch** downloads `Evict-Setup.exe`, checks it against the SHA-256 GitHub published for the release, installs it over the running copy and reopens Evict. **Check for Updates…** is in the ☰ menu; the PowerShell line updates it too.
- **Linux**: the **Update** button gives you the install command; run it again and the package manager upgrades Evict.

## Build from source

**macOS**: Apple's command line tools (`xcode-select --install`) or Xcode. The app runs on macOS 11 or later.

**Linux**: a C compiler and the WebKitGTK 4.1 (or 4.0) headers:

```sh
sudo apt install gcc libwebkit2gtk-4.1-dev      # Debian, Ubuntu
sudo dnf install gcc webkit2gtk4.1-devel        # Fedora
sudo pacman -S gcc webkit2gtk-4.1               # Arch
```

Then, on both:

```sh
git clone https://github.com/vassilidev/evict.git
cd evict
./install.sh
```

On macOS, `install.sh` builds Evict.app, installs it in `/Applications` (or `~/Applications`), then opens it; press **⌘R** to rescan. On Linux, it builds with `linux/build.sh` and installs Evict into `~/.local`, with its entry in the app menu. To update a build from source, `git pull && ./install.sh`.

**Windows**: Evict is cross-compiled with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) in an Ubuntu container, from macOS, Linux or WSL with Docker. `windows/build.sh package` makes `.build/Evict-Setup.exe` (x64, ARM64 and x86 inside), with the toolchain and the WebView2 SDK pinned by SHA-256.

## Command line

```sh
./evict              # the whole disk, report opened in your browser
./evict ~/Projects   # a single folder
```

The report opens with `open` on macOS and `xdg-open` on Linux. In the browser, a web page can't delete files, drive the file manager or run Claude Code, so the menu copies text instead: **Copy rm -rf command**, an `open -R` (macOS) or `xdg-open` (Linux) command for **Show in Finder**, a preview command for **Preview**, `tar -tvf` for **Show contents**, and the prompt for **Copy AI prompt**.

## Questions

**Why Full Disk Access on macOS?** Without it, macOS hides part of the disk (Mail, Messages, other apps' data) and interrupts the scan with a prompt per protected folder. You can still scan without it.

**Which Linux distributions work?** Any 64-bit glibc distribution (2.31+) with WebKitGTK 4.1 or 4.0. CI installs, starts and uninstalls Evict on Ubuntu 20.04, 22.04, 24.04 and 26.04, Debian 11, 12 and 13, Linux Mint 20, 21 and 22, Kali, Fedora 41 to 43, openSUSE Leap 15.6, Leap 16 and Tumbleweed, Arch, Manjaro, Rocky Linux and AlmaLinux 9 and 10, on x86_64 and ARM64; derivatives like Pop!_OS and EndeavourOS use the same packages. musl distributions like Alpine aren't supported.

**Which Windows versions work?** Windows 10 and 11, on Intel and AMD (x64), ARM64 and 32-bit PCs, from one installer; Windows Server works too. CI tests Windows Server 2022 and 2025 and Windows 11 ARM64 on every change. Windows 7 and 8.1 aren't supported: WebView2, which draws the window, stopped supporting them in 2023.

**Why does Windows warn about the installer?** It isn't code-signed yet, so SmartScreen doesn't know it: click **More info**, then **Run anyway**, or install with the PowerShell line, which checks the download against its SHA-256 on GitHub.

**Which Macs work?** Any Mac that runs macOS 11 Big Sur or later, Apple Silicon or Intel: most Macs from 2014 on.
