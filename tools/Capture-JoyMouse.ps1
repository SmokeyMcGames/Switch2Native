#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateRange(1,4)][int]$Slot,
    [ValidateRange(1,120)][int]$Seconds = 60,
    [ValidatePattern('^[a-z0-9-]+$')][string]$Label = 'surface'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$registryPath = 'HKLM:\SOFTWARE\Switch2Native'
$settings = Get-ItemProperty -LiteralPath $registryPath
if ($settings.JoyPairingMask) { throw 'A diagnostic already owns a Joy-Con.' }
$address = [uint64]$settings.("JoyAddress$Slot")
$product = [int]$settings.("JoyKind$Slot")
if (-not $address -or $address -gt 0xffffffffffff -or $product -notin 0x2066,0x2067) {
    throw 'Selected slot is not a configured Joy-Con.'
}
$kind = if ($product -eq 0x2066) { 'right' } else { 'left' }
$mask = 1 -shl ($Slot - 1)
$basePath = Join-Path $projectRoot "captures\joy-mouse-$kind-$Label"
$probe = Join-Path $projectRoot 'build\Release\JoyMouseProbe.exe'
if (-not (Test-Path -LiteralPath $probe)) { throw 'Build JoyMouseProbe first.' }
try {
    Set-ItemProperty -LiteralPath $registryPath -Name JoyPairingMask -Value $mask -Type DWord
    $released = $false
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        $status = & (Join-Path $projectRoot 'build\Release\JoyCheck.exe') --seconds 1
        if ($LASTEXITCODE) { throw 'Cannot read controller ownership.' }
        $ownerLine = $status | Select-String -Pattern 'owners=(\d+)' | Select-Object -First 1
        if ($ownerLine -and -not ([int]$ownerLine.Matches[0].Groups[1].Value -band $mask)) {
            $released = $true
            break
        }
    }
    if (-not $released) { throw 'Driver did not release the selected Joy-Con.' }
    $capture = Start-Process -FilePath $probe -ArgumentList @($address.ToString('x12'), $kind, $Seconds) `
        -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput "$basePath.csv" `
        -RedirectStandardError "$basePath-errors.log"
    if ($capture.ExitCode) { throw "Mouse capture failed: see $basePath-errors.log" }
} catch {
    $_ | Out-String | Set-Content "$basePath-failure.log"
    throw
} finally {
    if ((Get-ItemProperty -LiteralPath $registryPath).JoyPairingMask -eq $mask) {
        Set-ItemProperty -LiteralPath $registryPath -Name JoyPairingMask -Value 0 -Type DWord
    }
    [DateTime]::UtcNow.ToString('o') | Set-Content "$basePath-finished.txt"
}
