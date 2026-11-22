# Installs or updates Evict on Windows 10 and 11 (x64, ARM64 and x86), for the current user, no administrator rights:
#   irm https://vassilidev.github.io/evict/install.ps1 | iex
# Downloads Evict-Setup.exe from the newest GitHub release that has it, checks its SHA-256 against the release, then
# installs it silently. $env:EVICT_VERSION = '1.12.0' installs that version; $env:EVICT_FILE = 'path' installs an
# installer you already have. To uninstall: Settings > Apps > Evict, or
#   & "$env:LOCALAPPDATA\Programs\Evict\Uninstall.exe"
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # Windows PowerShell 5 downloads many times slower with its progress bar
# Windows PowerShell 5 on older Windows 10 doesn't offer TLS 1.2 by default, and GitHub requires it
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$repo = 'vassilidev/evict'
$setup = Join-Path ([IO.Path]::GetTempPath()) 'Evict-Setup.exe'

if ($env:EVICT_FILE) {
  Copy-Item $env:EVICT_FILE $setup -Force
  $version = 'local'
} else {
  $api = "https://api.github.com/repos/$repo/releases"
  $headers = @{Accept = 'application/vnd.github+json'}
  # Releases come newest first; the first one with the Windows installer is the latest for Windows
  $release = if ($env:EVICT_VERSION) { Invoke-RestMethod "$api/tags/v$($env:EVICT_VERSION.TrimStart('v'))" -Headers $headers }
             else { Invoke-RestMethod "$api`?per_page=10" -Headers $headers | Where-Object { $_.assets.name -contains 'Evict-Setup.exe' } | Select-Object -First 1 }
  $asset = $release.assets | Where-Object { $_.name -eq 'Evict-Setup.exe' }
  if (-not $asset) { throw 'No Windows installer in this release.' }
  $version = $release.tag_name.TrimStart('v')
  Write-Host "Downloading Evict $version..."
  Invoke-WebRequest $asset.browser_download_url -OutFile $setup -UseBasicParsing
  if ($asset.digest -and "sha256:$((Get-FileHash $setup -Algorithm SHA256).Hash.ToLower())" -ne $asset.digest) {
    Remove-Item $setup -Force
    throw 'The download does not match the release checksum: nothing was installed.'
  }
}

Write-Host 'Installing...'
$run = Start-Process $setup -ArgumentList '/S' -Wait -PassThru
Remove-Item $setup -Force -ErrorAction SilentlyContinue
if ($run.ExitCode -ne 0) { throw "The installer stopped (code $($run.ExitCode))." }
Write-Host "Evict $version is installed: open it from the Start menu."
