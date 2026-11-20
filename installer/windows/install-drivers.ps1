# LibreRamen: bind the WinUSB driver to the instrument's USB devices (run elevated).
#
#  - BaySpec spectrometers (BA8C:1688): always (the ID is unique to the instrument; also pre-installs
#    the driver when the instrument is not connected).
#  - Prolific PL2303 I/O board (067B:2303): only when exactly one PL2303 is connected and it is not
#    working with Prolific's driver (current Prolific drivers refuse the instrument's old HXA chip:
#    "PL2303HXA PHASED OUT SINCE 2012"). 067B:2303 is also used by ordinary USB-serial cables, which
#    must keep their COM-port driver, so this is never done blindly.
#  - The MCP2200 (04D8:00DF) needs nothing: it is used through Windows' own HID driver.
#
# Uses wdi-simple from libwdi (github.com/pbatard/libwdi, LGPL), the library behind Zadig.
param([switch]$Quiet)

$ErrorActionPreference = "Continue"
$ProgressPreference = "SilentlyContinue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$wdi = Join-Path $here "wdi-simple.exe"
$logDir = Join-Path $env:ProgramData "LibreRamen"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir "drivers.log"
$work = Join-Path $env:TEMP "libreramen-drivers"
$messages = New-Object System.Collections.Generic.List[string]

function Log($text) {
    "$(Get-Date -Format s)  $text" | Out-File -Append -Encoding utf8 $log
}
function Say($text) { Log $text; $messages.Add($text) }

function Devices($vid, $prodId) {
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match "^USB\\VID_$vid&PID_$prodId\\" } |
        ForEach-Object {
            $svc = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
            [pscustomobject]@{ Id = $_.InstanceId; Name = $_.FriendlyName; Status = $_.Status; Service = "$svc" }
        }
}

function Install($vid, $prodId, $name, $inf) {
    $dest = Join-Path $work $inf.Replace(".inf", "")
    $wargs = @("-n", $name, "-m", "LibreRamen", "-v", "0x$vid", "-p", "0x$prodId", "-t", "0",
               "-f", $inf, "-d", $dest)
    Log "wdi-simple $($wargs -join ' ')"
    $out = & $wdi @wargs 2>&1 | Out-String
    Log $out.Trim()
    return $LASTEXITCODE
}

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Output "Administrator rights are needed to install drivers."; exit 2 }
if (-not (Test-Path $wdi)) { Write-Output "wdi-simple.exe is missing next to this script."; exit 3 }
Log "---- driver set-up (LibreRamen)"
$rc = 0

# Spectrometers
$bs = @(Devices "BA8C" "1688")
if ($bs.Count -gt 0 -and -not ($bs | Where-Object { $_.Service -ne "WinUSB" })) {
    Say "Spectrometers: WinUSB already installed ($($bs.Count) device(s))."
} else {
    $r = Install "BA8C" "1688" "Raman spectrometer (LibreRamen)" "libreramen_spectrometer.inf"
    if ($r -eq 0) {
        Say ("Spectrometers: WinUSB installed" + $(if ($bs.Count -eq 0) { " (ready for when the instrument is connected)." } else { "." }))
    } else { Say "Spectrometers: driver installation failed (code $r, see $log)."; $rc = 1 }
}

# I/O board
$pl = @(Devices "067B" "2303")
if ($pl.Count -eq 0) {
    Say "I/O board: not connected. Connect the instrument, switch it on, then use Instrument > Repair USB drivers in LibreRamen."
} elseif ($pl.Count -gt 1) {
    Say "I/O board: $($pl.Count) Prolific USB-serial devices are connected. Unplug the other USB-serial cables, then use Instrument > Repair USB drivers."
} elseif ($pl[0].Service -eq "WinUSB") {
    Say "I/O board: WinUSB already installed."
} elseif ($pl[0].Status -eq "OK" -and $pl[0].Name -notmatch "PHASED OUT") {
    Say "I/O board: the connected Prolific device ($($pl[0].Name)) works with its own driver, so it is probably not the instrument's I/O board; left unchanged."
} else {
    $r = Install "067B" "2303" "Raman instrument I/O board (LibreRamen)" "libreramen_ioboard.inf"
    if ($r -eq 0) { Say "I/O board: WinUSB installed." }
    else { Say "I/O board: driver installation failed (code $r, see $log)."; $rc = 1 }
}

$summary = $messages -join "`n"
Write-Output $summary
if (-not $Quiet) {
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show($summary, "LibreRamen USB drivers") | Out-Null
}
exit $rc
