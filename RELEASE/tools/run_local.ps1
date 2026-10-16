<#
run_local.ps1: runs the heavy work on this computer (all CPU cores, your
GPU) and writes the results into dev\ in the project folder, where Claude
can read them. Windows version of run_local.sh; run_local.bat starts it with
a double-click.

  .\tools\run_local.ps1                  test + check + rip (the default)
  .\tools\run_local.ps1 test             build the Rust tools and run their tests
  .\tools\run_local.ps1 check            check every CoD3 archive against the format notes
  .\tools\run_local.ps1 rip              rip CoD3 textures, meshes and materials
  .\tools\run_local.ps1 recomp-test      build the recomp and play Saint-Lo on your GPU
  .\tools\run_local.ps1 rip -Waw "D:\Games\Call of Duty World at War"
  .\tools\run_local.ps1 waw360 -WawXbox360 "D:\ISOs\Call of Duty - World at War.iso"
                                   copy the fastfiles out of the Xbox 360 disc
                                   image and try ripping them
  .\tools\run_local.ps1 map-pack -WawMapPack "D:\Downloads\codwaw-v1.5_1.6.zip"
                                   take the map packs out of an official PC patch
                                   (never run) and try ripping them
  Steps can be combined: .\tools\run_local.ps1 check waw360 map-pack -WawXbox360 ... -WawMapPack ...

Options:
  -Jobs N       worker threads (default: every CPU core)
  -Iso PATH     CoD3 extracted folder (default: output\assets, the setup tool's copy)
  -Waw PATH     World at War folder; its fastfiles are ripped too
                (its mods\ folder is ignored)
  -Seconds N    how long recomp-test runs the game (default 600)
  -WawXbox360 PATH   World at War Xbox 360 disc image, for the waw360 step
  -WawMapPack PATH   patch download with map packs, for the map-pack step
                     (default: every patch in input\)
                     (where to get it: py COD_3_WAW_Setup-tool\cod3_waw_setup.py map-pack --list)

Output, all in dev\:
  logs\<step>.log, logs\summary.txt, logs\system.txt, logs\shots\*.png
  ripped\cod3\   textures\, meshes\, materials\
  ripped\waw\    raw files (scripts) of each fastfile
  ripped\waw360\ files\ (copied from the Xbox 360 image) and ripped\
  ripped\map-pack\ files\ (taken out of the patch) and ripped\; logs\map-pack-report.txt
  recomp-test\   the build made by recomp-test
Nothing outside the project folder is changed. Your ISO and WaW folder are only read.
#>
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Steps,
    [int]$Jobs = 0,
    [string]$Iso = "",
    [string]$Waw = "",
    [string]$WawXbox360 = "",
    [string]$WawMapPack = "",
    [int]$Seconds = 600
)

$ErrorActionPreference = "Continue"
$Root = Split-Path -Parent $PSScriptRoot
$Dev = Join-Path $Root "dev"
$Logs = Join-Path $Dev "logs"
$Engine = Join-Path $Root "engine"
$Ripped = Join-Path $Dev "ripped"
$OnWindows = ($env:OS -eq "Windows_NT")
$ExeSuffix = if ($OnWindows) { ".exe" } else { "" }
$Rip = Join-Path $Engine (Join-Path "target" (Join-Path "release" "rip$ExeSuffix"))

if (-not $Steps -or $Steps.Count -eq 0) { $Steps = @("test", "check", "rip") }
foreach ($s in $Steps) {
    if (@("test", "check", "rip", "recomp-test", "waw360", "map-pack") -notcontains $s) {
        Write-Host "unknown step: $s (see the top of run_local.ps1)"
        exit 2
    }
}
$JobsGiven = ($Jobs -gt 0)
if (-not $JobsGiven) { $Jobs = [Environment]::ProcessorCount }
if (-not $Iso) {
    # The extracted CoD3 disc: the setup tool's copy, or the older assets\ folder.
    $Iso = Join-Path $Root (Join-Path "output" "assets")
    $legacy = Join-Path $Root "assets"
    if (-not (Test-Path (Join-Path $Iso "sp")) -and (Test-Path (Join-Path $legacy "sp"))) { $Iso = $legacy }
}

New-Item -ItemType Directory -Force -Path $Logs | Out-Null
$Summary = Join-Path $Logs "summary.txt"
Add-Content -Path $Summary -Value ("run_local.ps1 {0} steps: {1} jobs: {2}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), ($Steps -join " "), $Jobs)

# What this computer is, for reading the logs later.
& {
    "date: $(Get-Date)"
    "system: $([Environment]::OSVersion.VersionString)"
    "cpu threads: $([Environment]::ProcessorCount)"
    if ($OnWindows) {
        Get-CimInstance Win32_Processor | ForEach-Object { "cpu: $($_.Name)" }
        Get-CimInstance Win32_VideoController | ForEach-Object { "gpu: $($_.Name) driver $($_.DriverVersion)" }
        $mem = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
        "memory: {0:N1} GB" -f ($mem / 1GB)
    }
} *>&1 | Out-File -FilePath (Join-Path $Logs "system.txt") -Encoding utf8

# Set to $false by a step block when something in it fails.
$script:StepOk = $true

# Runs one step, logging its output and how it went.
function Invoke-Step([string]$Name, [scriptblock]$Block) {
    $log = Join-Path $Logs "$Name.log"
    Write-Host "== $Name (log: dev\logs\$Name.log)"
    $start = Get-Date
    $script:StepOk = $true
    try {
        & $Block *>&1 | Out-File -FilePath $log -Encoding utf8
    } catch {
        $_ | Out-File -FilePath $log -Encoding utf8 -Append
        $script:StepOk = $false
    }
    $took = [int]((Get-Date) - $start).TotalSeconds
    Get-Content $log -Tail 5 | ForEach-Object { Write-Host $_ }
    $result = if ($script:StepOk) { "ok" } else { "FAILED" }
    $line = "  ${Name}: $result in ${took}s"
    Write-Host $line
    Add-Content -Path $Summary -Value $line
    return $script:StepOk
}

# Runs a program and records a failure if it exits with an error.
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { $script:StepOk = $false }
}

function Find-Cargo {
    $cmd = Get-Command cargo -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $home_cargo = Join-Path $HOME (Join-Path ".cargo" (Join-Path "bin" "cargo$ExeSuffix"))
    if (Test-Path $home_cargo) { return $home_cargo }
    return $null
}
$script:Cargo = Find-Cargo

# Installs Rust through the setup tool when it's missing.
function Install-Rust {
    "Rust isn't installed; installing it now (into your user folder, no admin rights needed)..."
    $python = Get-Python
    & $python (Join-Path $Root (Join-Path "COD_3_WAW_Setup-tool" "cod3_waw_setup.py")) doctor --fix --no-recomp
    $script:Cargo = Find-Cargo
}

function Build-Tools([bool]$WithTests) {
    if (-not $script:Cargo) { Install-Rust }
    if (-not $script:Cargo) {
        "Rust couldn't be installed automatically. Install it from https://rustup.rs, then run this again."
        $script:StepOk = $false
        return
    }
    Push-Location $Engine
    try {
        Invoke-Checked $script:Cargo @("build", "--release", "--jobs", "$Jobs")
        if ($WithTests -and $script:StepOk) { Invoke-Checked $script:Cargo @("test", "--release", "--jobs", "$Jobs") }
    } finally {
        Pop-Location
    }
}

# Removes an output folder this script made before (it holds .rip-output),
# never anything else.
function Reset-Output([string]$Folder) {
    if (Test-Path (Join-Path $Folder ".rip-output")) { Remove-Item -Recurse -Force -LiteralPath $Folder }
    New-Item -ItemType Directory -Force -Path $Folder | Out-Null
    New-Item -ItemType File -Force -Path (Join-Path $Folder ".rip-output") | Out-Null
}

# check and rip read the extracted disc, not the .iso itself.
function Test-Extracted {
    if (Test-Path (Join-Path $Iso "sp") -PathType Container) { return $true }
    Write-Host "$(Join-Path $Iso 'sp') not found. check and rip need the folder extracted from the ISO: build once with the setup tool (output\assets), or give -Iso PATH."
    return $false
}

function Save-Screenshot([string]$Path) {
    if (-not $OnWindows) { return }
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    $bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $bitmap.Size)
    $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose()
    $bitmap.Dispose()
}

function Test-Recomp {
    $out = Join-Path $Dev "recomp-test"
    $setup = Join-Path $Root (Join-Path "COD_3_WAW_Setup-tool" "setup.bat")
    "Building the recomp into $out (the first build takes a while)..."
    # The setup tool picks its compile threads from the RAM (about 2 GB each);
    # -Jobs is passed on only when given explicitly.
    $setupArgs = @("build", "--iso", $Iso, "--output", $out, "--keep-sdk", "--no-waw")
    if ($JobsGiven) { $setupArgs += @("--jobs", "$Jobs") }
    Invoke-Checked $setup $setupArgs
    if (-not $script:StepOk) { return }
    $exe = Join-Path $out (Join-Path "bin" "cod3$ExeSuffix")
    if (-not (Test-Path $exe)) { "no game executable at $exe"; $script:StepOk = $false; return }
    $shots = Join-Path $Logs "shots"
    New-Item -ItemType Directory -Force -Path $shots | Out-Null
    Get-ChildItem $shots -Filter "recomp-*.png" -ErrorAction SilentlyContinue | Remove-Item -Force
    "Running Saint-Lo for ${Seconds}s; screenshots every 30s into dev\logs\shots\"
    $gameLog = Join-Path $Logs "recomp-game.log"
    $arguments = @("--fullscreen=false", "--log_file=`"$gameLog`"", "--cod3_autoexec=`"spmap saint_lo`"",
        "--cod3_autoexec_delay=60", "--cod3_debug_frames=true")
    $game = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $out -PassThru
    $waited = 0
    while (-not $game.HasExited -and $waited -lt $Seconds) {
        Start-Sleep -Seconds 30
        $waited += 30
        Save-Screenshot (Join-Path $shots ("recomp-{0:D4}.png" -f $waited))
    }
    if (-not $game.HasExited) {
        Stop-Process -Id $game.Id -Force
        "stopped the game after ${waited}s"
    } else {
        "the game exited by itself after about ${waited}s (code $($game.ExitCode))"
    }
}

function Get-Python {
    if (Get-Command py -ErrorAction SilentlyContinue) { return "py" }
    return "python"
}

# World at War for Xbox 360: lists the disc image, copies its fastfiles out
# (the image is only read) and tries the ripper on each one.
function Invoke-Waw360 {
    if (-not $WawXbox360) { "Give the disc image: -WawXbox360 PATH"; $script:StepOk = $false; return }
    $tool = Join-Path $Root (Join-Path "COD_3_WAW_Setup-tool" "cod3_waw_setup.py")
    $python = Get-Python
    $out = Join-Path $Ripped "waw360"
    Reset-Output $out
    & $python $tool xiso $WawXbox360 | Out-File -FilePath (Join-Path $Logs "waw360-files.txt") -Encoding utf8
    Invoke-Checked $python @($tool, "xiso", $WawXbox360, "--extract", (Join-Path $out "files"), "--only", "*.ff")
    if (-not $script:StepOk) { return }
    $files = Get-ChildItem -Path (Join-Path $out "files") -Recurse -Filter "*.ff" | Sort-Object FullName
    $ripped = 0
    foreach ($ff in $files) {
        & $Rip waw-ff $ff.FullName (Join-Path (Join-Path $out "ripped") $ff.BaseName)
        if ($LASTEXITCODE -eq 0) { $ripped++ }
    }
    "ripped $ripped of $($files.Count) Xbox 360 fastfiles"
}

# Map packs: describes the patch download (dev\logs\map-pack-report.txt), takes
# its fastfiles and .iwd files out (its installer is never run) and tries the
# ripper on each fastfile.
function Invoke-MapPack {
    $tool = Join-Path $Root (Join-Path "COD_3_WAW_Setup-tool" "cod3_waw_setup.py")
    $python = Get-Python
    # The download given with -WawMapPack, or every patch in input\.
    if ($WawMapPack) {
        $downloads = @($WawMapPack)
    } else {
        $downloads = @(Get-ChildItem -Path (Join-Path $Root "input") -File -Depth 1 -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like "codwaw-v*.zip" -or $_.Name -like "codwaw*patch*.exe" } |
            Sort-Object Name | ForEach-Object { $_.FullName })
    }
    if ($downloads.Count -eq 0) {
        "No map pack download: put the patches in input\ or give one with -WawMapPack PATH"
        & $python $tool map-pack --list
        $script:StepOk = $false
        return
    }
    $out = Join-Path $Ripped "map-pack"
    Reset-Output $out
    $report = Join-Path $Logs "map-pack-report.txt"
    Set-Content -Path $report -Value "" -Encoding utf8
    foreach ($download in $downloads) {
        & $python $tool map-pack $download *>&1 | Out-File -FilePath $report -Encoding utf8 -Append
        Invoke-Checked $python @($tool, "map-pack", $download, "--extract", (Join-Path $out "files"))
    }
    $files = Get-ChildItem -Path (Join-Path $out "files") -Recurse -Filter "*.ff" -ErrorAction SilentlyContinue |
        Sort-Object FullName
    $ripped = 0
    foreach ($ff in $files) {
        & $Rip waw-ff $ff.FullName (Join-Path (Join-Path $out "ripped") $ff.BaseName)
        if ($LASTEXITCODE -eq 0) { $ripped++ }
    }
    "ripped $ripped of $(@($files).Count) map pack fastfiles"
}

# check and rip use the freshly built tools even when "test" isn't asked for.
$NeedsTools = @("check", "rip", "waw360", "map-pack") | Where-Object { $Steps -contains $_ }
if ($Steps -notcontains "test" -and $NeedsTools) {
    if (-not (Invoke-Step "build" { Build-Tools $false })) { exit 1 }
}

foreach ($s in $Steps) {
    switch ($s) {
        "test" { if (-not (Invoke-Step "test" { Build-Tools $true })) { exit 1 } }
        "check" {
            if (Test-Extracted) {
                Invoke-Step "check" { Invoke-Checked $Rip @("cod3-check", (Join-Path $Iso "sp"), "--jobs", "$Jobs") } | Out-Null
            }
        }
        "rip" {
            if (Test-Extracted) {
                Invoke-Step "rip-cod3" {
                    $cod3Out = Join-Path $Ripped "cod3"
                    Reset-Output $cod3Out
                    Invoke-Checked $Rip @("cod3-all", (Join-Path $Iso "sp"), $cod3Out, "--jobs", "$Jobs")
                } | Out-Null
            }
            if ($Waw) {
                Invoke-Step "rip-waw" {
                    $wawOut = Join-Path $Ripped "waw"
                    Reset-Output $wawOut
                    # Fastfiles live under zone\; the mods\ folder is the user's own and skipped.
                    $sep = [IO.Path]::DirectorySeparatorChar
                    Get-ChildItem -Path (Join-Path $Waw "zone") -Recurse -Filter "*.ff" |
                        Where-Object { $_.FullName -notlike "*${sep}mods${sep}*" } |
                        Sort-Object FullName |
                        ForEach-Object { Invoke-Checked $Rip @("waw-ff", $_.FullName, (Join-Path $wawOut $_.BaseName)) }
                } | Out-Null
            }
        }
        "recomp-test" { Invoke-Step "recomp-test" { Test-Recomp } | Out-Null }
        "waw360" { Invoke-Step "waw360" { Invoke-Waw360 } | Out-Null }
        "map-pack" { Invoke-Step "map-pack" { Invoke-MapPack } | Out-Null }
    }
}
Write-Host "Done. Results: $Summary"
