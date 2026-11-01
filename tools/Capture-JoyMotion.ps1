#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateRange(1,4)][int]$Slot,
    [ValidateRange(1,120)][int]$Seconds = 15,
    [ValidateSet(6,7,8,10,12)][int]$IntervalUnits = 6,
    [ValidatePattern('^[a-z0-9-]+$')][string]$Label = 'still'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$registryPath = 'HKLM:\SOFTWARE\Switch2Native'
$record = Get-Content -LiteralPath (Join-Path $env:ProgramData 'Switch2Native\installation.json') -Raw | ConvertFrom-Json
if ($record.JoyAddresses.Count -lt $Slot) { throw 'This Joy-Con slot is not configured.' }
$address = $record.JoyAddresses[$Slot - 1]
$kind = $record.JoyKinds[$Slot - 1]
if ($address -notmatch '^[0-9a-fA-F]{12}$' -or $kind -notin 'left','right') { throw 'Invalid saved Joy-Con configuration.' }
$settings = Get-ItemProperty -LiteralPath $registryPath
if ($settings.JoyPairingMask) { throw 'A pairing or diagnostic operation already owns a Joy-Con.' }
$savedAddress = $settings.("JoyAddress$Slot")
if ($savedAddress -ne [Convert]::ToUInt64($address, 16)) { throw 'The installed address no longer matches the installation record.' }
$mask = 1 -shl ($Slot - 1)
$probe = Join-Path $projectRoot 'build\Release\BleMotionProbe.exe'
$statusTool = Join-Path $projectRoot 'build\Release\JoyCheck.exe'
$captureDirectory = Join-Path $projectRoot 'captures'
$baseName = "joy-packed-$kind-$Label"
New-Item -ItemType Directory -Path $captureDirectory -Force | Out-Null
$logPath = Join-Path $captureDirectory "$baseName.log"
try {
    Set-ItemProperty -LiteralPath $registryPath -Name JoyPairingMask -Value $mask -Type DWord
    $released = $false
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        $status = & $statusTool --seconds 1
        if ($LASTEXITCODE) { throw 'Cannot read coordinator ownership.' }
        $ownerLine = $status | Select-String -Pattern 'owners=(\d+)' | Select-Object -First 1
        if ($ownerLine -and -not ([int]$ownerLine.Matches[0].Groups[1].Value -band $mask)) {
            $released = $true
            break
        }
    }
    if (-not $released) { throw 'The installed driver did not release the selected controller.' }
    & $probe --address $address --kind $kind --standalone --interval-units $IntervalUnits --seconds $Seconds --capture (Join-Path $captureDirectory "$baseName.txt") 2>&1 |
        Out-File -LiteralPath $logPath -Encoding utf8
    $probeExit = $LASTEXITCODE
    "Probe exit: $probeExit" | Add-Content -LiteralPath $logPath -Encoding utf8
    if ($probeExit) { throw "Packed capture failed with exit $probeExit. See $logPath" }
} finally {
    # Release only this diagnostic's reservation; preserve a concurrently changed mask.
    if ((Get-ItemProperty -LiteralPath $registryPath).JoyPairingMask -eq $mask) {
        Set-ItemProperty -LiteralPath $registryPath -Name JoyPairingMask -Value 0 -Type DWord
    }
}
