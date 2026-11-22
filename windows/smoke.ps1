# Evict's whole life on one Windows machine: install with the website's install script, check the scanner on a
# fixture and on the whole computer, start the app (it must scan and show its map), then uninstall and check that
# nothing is left. CI runs it on Windows Server 2022 and 2025 (x64) and Windows 11 (ARM64).
# Usage: windows/smoke.ps1 <Evict-Setup.exe>   an installer built by CI (its x86 build is checked too, from .build)
#        windows/smoke.ps1                     the published release, through the website (EVICT_VERSION)
param([string]$Setup)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [Text.Encoding]::UTF8  # the scanner writes UTF-8
$dir = "$env:LOCALAPPDATA\Programs\Evict"
$data = "$env:LOCALAPPDATA\Evict"
$shortcut = "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Evict.lnk"
$uninstall = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Evict'
$shots = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }
function Fail($message) { Screenshot 'failure'; throw "FAIL: $message" }
function Screenshot($name) {
  Add-Type -AssemblyName System.Windows.Forms, System.Drawing
  $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $image = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
  [System.Drawing.Graphics]::FromImage($image).CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
  $image.Save("$shots\evict-$name.png")
}

# WebView2 comes with Windows 11 and current Windows 10; server images can lack it (Evict would offer to install it)
$runtime = 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}'
if (-not ((Get-ItemProperty "HKLM:\$runtime" -ErrorAction SilentlyContinue).pv -or (Get-ItemProperty "HKCU:\$runtime" -ErrorAction SilentlyContinue).pv)) {
  Write-Host 'Installing the WebView2 Runtime'
  Invoke-WebRequest 'https://go.microsoft.com/fwlink/p/?LinkId=2124703' -OutFile "$env:TEMP\webview2.exe" -UseBasicParsing
  Start-Process "$env:TEMP\webview2.exe" -ArgumentList '/silent', '/install' -Wait
}

# Install
$watch = [Diagnostics.Stopwatch]::StartNew()
if ($Setup) {
  $env:EVICT_FILE = (Resolve-Path $Setup).Path
  Invoke-Expression (Get-Content -Raw docs/install.ps1)
  Remove-Item Env:EVICT_FILE
} else {
  Invoke-RestMethod https://vassilidev.github.io/evict/install.ps1 | Invoke-Expression
}
foreach ($f in 'Evict.exe', 'evict-scan.exe', 'index.html', 'WebView2Loader.dll', 'Uninstall.exe') {
  if (-not (Test-Path "$dir\$f")) { Fail "not installed: $f" }
}
$watch.Stop()
Write-Host ("Installed in {0:N1} s" -f $watch.Elapsed.TotalSeconds)
if (-not (Test-Path $shortcut)) { Fail 'no Start menu entry' }
if (-not (Test-Path $uninstall)) { Fail 'not listed in Settings > Apps' }
# The icons as Windows shows them, kept with the screenshots: a broken icon comes out as Windows' generic one
Add-Type -AssemblyName System.Drawing
$icons = @{app = "$dir\Evict.exe"}
if ($Setup) { $icons.setup = (Resolve-Path $Setup).Path }
foreach ($name in $icons.Keys) { [System.Drawing.Icon]::ExtractAssociatedIcon($icons[$name]).ToBitmap().Save("$shots\evict-icon-$name.png") }
$version = (Get-Item "$dir\Evict.exe").VersionInfo.ProductVersion
if ($env:EVICT_VERSION -and $version -ne $env:EVICT_VERSION.TrimStart('v')) { Fail "expected Evict $env:EVICT_VERSION, got $version" }
$arch = $env:PROCESSOR_ARCHITECTURE
Write-Host "Evict $version installed ($arch)"

# The scanner on a fixture: a big file, a small one, a hard link to the big one (counted once) and a tiny file
function Check-Scanner($scanner) {
  $t = Join-Path $env:TEMP 'evict-fixture'
  Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
  New-Item -ItemType Directory "$t\a\b", "$t\c" | Out-Null
  $bytes = New-Object byte[] 3145728
  (New-Object Random).NextBytes($bytes)
  [IO.File]::WriteAllBytes("$t\a\big", $bytes)
  [IO.File]::WriteAllBytes("$t\a\b\small", [byte[]]$bytes[0..10239])
  New-Item -ItemType HardLink -Path "$t\c\hardlink" -Target "$t\a\big" | Out-Null
  Set-Content "$t\c\tiny" 'hi'
  $text = & $scanner $t 2> $null
  $json = $text | ConvertFrom-Json
  $got = [int64]$json.tree[1]
  # 3 MiB + 12 KiB allocated; the tiny file lives inside the MFT record (0) or takes one cluster
  if ($got -lt 3158016 -or $got -gt 3162112) { Fail "$scanner counted $got bytes, expected 3158016 to 3162112" }
  if (([regex]::Matches($text, '"(big|hardlink)",')).Count -ne 1) { Fail "$scanner didn't count the hard link once" }
  if ($json.os -ne 'win' -or $json.home -notmatch '^/[A-Z]:/' -or $json.trash -notmatch '^/[A-Z]:/\$Recycle\.Bin/S-') { Fail "$scanner report header: $($text.Substring(0, 200))" }
  Remove-Item $t -Recurse -Force
  Write-Host "Scanner OK: $got bytes ($scanner)"
}
Check-Scanner "$dir\evict-scan.exe"
if ($Setup -and (Test-Path .build\windows\x86\evict-scan.exe)) { Check-Scanner (Resolve-Path .build\windows\x86\evict-scan.exe).Path }

# The whole computer, every fixed drive, cold then warm
function Scan-All($label) {
  $watch = [Diagnostics.Stopwatch]::StartNew()
  $text = & "$dir\evict-scan.exe" --home $env:USERPROFILE / 2> $null
  $watch.Stop()
  $json = $text | ConvertFrom-Json
  $used = $json.total - $json.free
  Write-Host ("Whole computer, {0}: {1:N0} items, {2:N2} GB of {3:N1} GB used seen, in {4:N1} s, root {5}" -f $label, $json.count, ($json.tree[1] / 1e9), ($used / 1e9), $watch.Elapsed.TotalSeconds, $json.tree[0])
  if ($json.count -lt 10000 -or $json.tree[1] -lt $used / 4) { Fail 'the whole-computer scan saw too little' }
  [pscustomobject]@{total = [double]$json.tree[1]; secs = $watch.Elapsed.TotalSeconds; tree = $json.tree}
}
# Folder sizes by path, four levels deep, to see where two scans differ
function Sizes($node, $path, $depth, $out) {
  if ($null -eq $node[0]) { return }
  $p = if ($null -eq $path) { $node[0] } elseif ($path -eq '/') { "/$($node[0])" } else { "$path/$($node[0])" }
  $out[$p] = [double]$node[1]
  if ($depth -lt 4 -and $node.Count -ge 3 -and $node[2] -is [array]) { foreach ($k in $node[2]) { Sizes $k $p ($depth + 1) $out } }
}
function Compare-Scans($a, $b, $label) {
  $sa = @{}; $sb = @{}
  Sizes $a.tree $null 0 $sa
  Sizes $b.tree $null 0 $sb
  $rows = foreach ($p in ($sa.Keys + $sb.Keys | Sort-Object -Unique)) {
    $d = $sa[$p] - $sb[$p]
    if ([math]::Abs($d) -gt 50e6) { [pscustomobject]@{path = $p; a = $sa[$p]; b = $sb[$p]; diff = $d} }
  }
  Write-Host "Biggest differences, $label (MB):"
  $rows | Sort-Object { -[math]::Abs($_.diff) } | Select-Object -First 25 |
    ForEach-Object { Write-Host ("  {0,10:N0} {1,10:N0} {2,10:N0}  {3}" -f ($_.a / 1e6), ($_.b / 1e6), ($_.diff / 1e6), $_.path) }
}
# Windows and System32 are cached: walked cold (filling the cache), read from the cache, then walked again
$cache = "$data\system.cache"
Remove-Item $cache -ErrorAction SilentlyContinue
$cold = Scan-All 'cold, walked'
if (-not (Test-Path $cache)) { Fail 'the scan wrote no system folder cache' }
$cached = Scan-All 'warm, cached'
Remove-Item $cache
$walked = Scan-All 'warm, walked'
Compare-Scans $cached $walked 'cached vs walked'
Compare-Scans $cold $walked 'cold walked vs warm walked'
# Windows Server changes about a gigabyte in the background between scans: the cache must not add more than that
if ([math]::Abs($cached.total - $walked.total) -gt 1.5e9) { Fail "cached total $($cached.total) differs from walked total $($walked.total)" }
Write-Host ("System folder cache OK: {0:N1} s cached vs {1:N1} s walked" -f $cached.secs, $walked.secs)

# Start: the app scans, writes its report and shows the map (the page reports its boxes through EVICT_SMOKE)
$proof = "$env:TEMP\evict-smoke.txt"
Remove-Item $proof -ErrorAction SilentlyContinue
$env:EVICT_SMOKE = $proof
$app = Start-Process "$dir\Evict.exe" -PassThru
Remove-Item Env:EVICT_SMOKE
for ($i = 0; $i -lt 240 -and -not (Test-Path $proof); $i++) { Start-Sleep -Milliseconds 500 }
Start-Sleep -Seconds 2
Screenshot ($arch.ToLower())
if (-not (Test-Path $proof)) { Fail 'the map never showed' }
$boxes = [int]((Get-Content $proof) -replace 'boxes=', '')
if ($boxes -lt 1) { Fail 'the map is empty' }
if (-not (Test-Path "$data\evict.html")) { Fail 'no report' }
if ($app.HasExited) { Fail 'the app quit' }
$secs = [regex]::Match((Get-Content -Raw "$data\evict.html"), '"secs":([0-9.]+)').Groups[1].Value
Write-Host "App OK: map shown with $boxes boxes, its own scan (warm) took $secs s"
$app.CloseMainWindow() | Out-Null
if (-not $app.WaitForExit(15000)) { Stop-Process $app -Force; Fail "the app didn't close" }
Get-Process msedgewebview2 -ErrorAction SilentlyContinue | Wait-Process -Timeout 20 -ErrorAction SilentlyContinue

# Uninstall: nothing may be left (the uninstaller runs from a temporary copy, so wait for it)
Start-Process "$dir\Uninstall.exe" -ArgumentList '/S' -Wait
for ($i = 0; $i -lt 60 -and ((Test-Path $dir) -or (Test-Path $data)); $i++) { Start-Sleep -Milliseconds 500 }
foreach ($left in $dir, $data, $shortcut, $uninstall) {
  if (Test-Path $left) { Fail "left behind after uninstall: $left" }
}
Write-Host "OK $((Get-CimInstance Win32_OperatingSystem).Caption), ${arch}: installed, scanned, map shown, uninstalled cleanly"
