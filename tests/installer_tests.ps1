$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'installer\SetupPolicy.ps1')
$checks = 0
function Assert-Equal($Expected, $Actual) {
    if ($Expected -ne $Actual) { throw "Expected $Expected; got $Actual" }
    $script:checks++
}
function Assert-Rejected([scriptblock]$Operation) {
    $rejected = $false
    try { & $Operation | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw 'An unsafe setup action was accepted.' }
    $script:checks++
}
$oldNative = @([version]'0.3.9.2') * 12
$sensors = @([version]'0.3.8.5') * 5
$mixedNative = @([version]'0.3.9.3') * 2 + @([version]'0.3.8.5') * 10
Assert-Equal 'install' (Get-SetupAction '0.3.9.3' '0.3.8.5' @() @() '')
Assert-Equal 'transport' (Get-SetupAction '0.3.9.3' '0.3.8.5' $oldNative $sensors 'installed')
Assert-Equal 'maintain' (Get-SetupAction '0.3.9.3' '0.3.8.5' $mixedNative $sensors 'installed')
Assert-Equal 'update' (Get-SetupAction '0.3.9.4' '0.3.8.6' $oldNative $sensors 'installed')
Assert-Rejected { Get-SetupAction '0.3.9.1' '0.3.8.5' $oldNative $sensors 'installed' }
Assert-Rejected { Get-SetupAction '0.3.9.3' '0.3.8.4' $oldNative $sensors 'installed' }
Assert-Rejected { Get-SetupAction '0.3.9.3' '0.3.8.5' $oldNative $sensors 'update-failed' }
Assert-Rejected { Get-SetupAction '0.3.9.3' '0.3.8.5' @([version]'0.3.9.2') $sensors 'installed' }
Assert-Rejected { Get-SetupAction '0.3.9.3' '0.3.8.5' (@([version]'0.3.7.0') * 12) $sensors 'installed' }
foreach ($path in @(Get-ChildItem (Join-Path $root 'tools') -Filter '*.ps1') +
                   @(Get-ChildItem (Join-Path $root 'installer') -Filter '*.ps1')) {
    $errors = $null
    [Management.Automation.Language.Parser]::ParseFile($path.FullName, [ref]$null, [ref]$errors) | Out-Null
    if ($errors) { throw "$($path.Name): $errors" }
    $checks++
}
$helper = Join-Path $root 'out\driver\Switch2DeviceSetup.exe'
foreach ($inf in 'Switch2Native.inf','Switch2Sensors.inf') {
    $path = Join-Path $root "driver\$inf"
    $ids = Select-String -LiteralPath $path -Pattern ',(ROOT\\SWITCH2[A-Z0-9]+)$' | ForEach-Object { $_.Matches[0].Groups[1].Value }
    foreach ($id in $ids) {
        & $helper check $path $id
        Assert-Equal 0 $LASTEXITCODE
    }
}
$ErrorActionPreference = 'Continue' # Windows PowerShell wraps expected native stderr as an error record.
& $helper check (Join-Path $root 'driver\Switch2Native.inf') 'ROOT\UNRELATED' 2>$null
Assert-Equal 2 $LASTEXITCODE
& $helper check (Join-Path $root 'driver\Switch2Sensors.inf') 'ROOT\SWITCH2NATIVE' 2>$null
Assert-Equal 2 $LASTEXITCODE
Write-Output "$checks installer checks passed. No device, service or certificate stores were changed."
