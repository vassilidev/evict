# Code signing policy

<sub>[README](../README.md) · [Features](features.md) · [Install](install.md) · [How it works](internals.md) · [Code signing](code-signing.md) · [Website](https://vassilidev.github.io/evict/)</sub>

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org).

## What is signed

- **Windows**: `Evict-Setup.exe` and the programs it installs, `Evict.exe` and `evict-scan.exe`, are signed through SignPath Foundation once the project is accepted; until then the installer is unsigned, and SmartScreen warns about it.
- **macOS**: `Evict.app` and `Evict.dmg` are signed with the developer's Apple Developer ID and notarized by Apple.
- **Linux**: packages aren't signed; the [download page](https://vassilidev.github.io/evict/download.html) lists the SHA-256 checksum of each one.

Every signed file is built from this public repository by the [release workflow](../.github/workflows/release.yml), on GitHub-hosted runners, from a version tag. Nothing is built or signed on a personal computer, and every release is approved by hand before it's signed.

## Team and roles

- **Committers and reviewers**: [vassilidev](https://github.com/vassilidev). Changes from other contributors come through pull requests, reviewed before they're merged, and built and tested by CI on macOS, Windows and Linux.
- **Approvers**: [vassilidev](https://github.com/vassilidev) approves each signing request.

Every team member uses multi-factor authentication on GitHub and SignPath.

## Privacy policy

Evict reads your disk on your computer and keeps the results there: no account, no telemetry, no analytics. It only uses the network for these:

- **Update checks**: at launch and every 6 hours, Evict reads the list of releases from GitHub's public API (`api.github.com`). When you choose to update, it downloads the new version from GitHub. Nothing about you or your disk is sent; GitHub receives the request like any visit to its website.
- **Ask Claude and Quick wins**, only when you click them: the prompt shown in the app (names and sizes from the scan, never file contents) goes to your own [Claude Code](https://claude.com/claude-code), which you installed and signed in to yourself.
- **WebView2 Runtime** (Windows), only when it's missing and you accept: Evict downloads Microsoft's installer for it.

Uninstalling Evict removes the app and its scan reports: see [uninstalling](install.md#updates).
