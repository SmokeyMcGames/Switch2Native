#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [ValidatePattern('^$|^[0-9a-fA-F]{12}$')][string]$BluetoothAddress = '',
    [Parameter(Mandatory)][switch]$TrustLocalDevelopmentCertificate,
    [ValidatePattern('^$|^[0-9a-fA-F]{12}$')][string]$JoyLeftAddress = '',
    [ValidatePattern('^$|^[0-9a-fA-F]{12}$')][string]$JoyRightAddress = '',
    [ValidatePattern('^[0-9a-fA-F]{12}$')][string[]]$JoyAddresses = @(),
    [ValidateSet('left','right')][string[]]$JoyKinds = @(),
    [string]$LogDirectory = ''
)
$ErrorActionPreference = 'Stop'
if (-not $TrustLocalDevelopmentCertificate) { throw 'Local certificate trust must be explicitly selected.' }
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $LogDirectory) { $LogDirectory = Join-Path $projectRoot 'captures' }
New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
Start-Transcript -Path (Join-Path $LogDirectory 'driver-install.log') -Append | Out-Null
$package = Join-Path $projectRoot 'out\driver'
$stateDirectory = Join-Path $env:ProgramData 'Switch2Native'
$stateFile = Join-Path $stateDirectory 'installation.json'
$hasJoyCons = Select-String -LiteralPath (Join-Path $package 'Switch2Native.inf') -SimpleMatch 'ROOT\SWITCH2JOYHUB' -Quiet
$knownJoyRoles = @('ProHub','JoyHub','JoyPad1','JoyPad2','JoyPad3','JoyPad4','JoyMotion1','JoyMotion2','JoyMotion3','JoyMotion4','JoySensor1','JoySensor2','JoySensor3','JoySensor4','ProSensor')
$hasTimestampedSensors = Test-Path -LiteralPath (Join-Path $package 'Switch2Sensors.inf')
$joyRoles = @($knownJoyRoles | Where-Object {
    if ($_ -like '*Sensor*') { $hasTimestampedSensors -and (Select-String -LiteralPath (Join-Path $package 'Switch2Sensors.inf') -SimpleMatch "ROOT\SWITCH2$($_.ToUpperInvariant())" -Quiet) }
    else { Select-String -LiteralPath (Join-Path $package 'Switch2Native.inf') -SimpleMatch "ROOT\SWITCH2$($_.ToUpperInvariant())" -Quiet }
})
if (-not $JoyAddresses.Count) {
    if ($JoyLeftAddress) { $JoyAddresses += $JoyLeftAddress; $JoyKinds += 'left' }
    if ($JoyRightAddress) { $JoyAddresses += $JoyRightAddress; $JoyKinds += 'right' }
}
if ($JoyAddresses.Count -gt 4 -or $JoyAddresses.Count -ne $JoyKinds.Count) { throw 'Provide one kind for each Joy-Con address, up to four controllers.' }
if (@($JoyAddresses | Select-Object -Unique).Count -ne $JoyAddresses.Count) { throw 'Joy-Con addresses must be unique.' }
function Get-Switch2NativeDevice {
    param([string]$HardwareId = 'ROOT\SWITCH2NATIVE')
    Get-PnpDevice -Class HIDClass,Sensor -ErrorAction SilentlyContinue | Where-Object {
        $_.InstanceId -like 'ROOT\*' -and
        (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_HardwareIds -ErrorAction SilentlyContinue).Data -contains $HardwareId
    }
}
if (Test-Path -LiteralPath $stateFile) {
    $previousRecord = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
    if ($previousRecord.Status -eq 'installed' -or $previousRecord.DeviceId) { throw 'An installed virtual controller already exists. Use Update-Driver.ps1 for routine updates.' }
}
$existing = @(Get-Switch2NativeDevice)
$existing += @(Get-Switch2NativeDevice -HardwareId 'ROOT\SWITCH2MOTION')
foreach ($role in $knownJoyRoles) { $existing += @(Get-Switch2NativeDevice -HardwareId "ROOT\SWITCH2$($role.ToUpperInvariant())") }
if ($existing.Count) { throw 'A Switch2Native virtual controller already exists. Inspect it before installing another.' }
$manifest = Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw | ConvertFrom-Json
if ($previousRecord -and $previousRecord.CertificateThumbprint -ne $manifest.CertificateThumbprint) { throw 'An unfinished installation uses another certificate. Uninstall it first.' }
$packageFiles = @('Switch2NativeDriver.dll','Switch2Native.inf','Switch2Native.cat','Switch2Native.cer','Switch2DeviceSetup.exe')
if ($hasJoyCons) { $packageFiles += @('Switch2Manager.exe','Switch2JoyCons.exe') }
if ($hasTimestampedSensors) { $packageFiles += @('Switch2SensorDriver.dll','Switch2Sensors.inf','Switch2Sensors.cat') }
foreach ($file in $packageFiles) {
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $package $file)).Hash
    if ($actual -ne $manifest.Files.$file) { throw "Package changed after signing: $file" }
}
$certificateFile = Join-Path $package 'Switch2Native.cer'
$certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($certificateFile)
if ($certificate.Thumbprint -ne $manifest.CertificateThumbprint -or $certificate.Subject -ne 'CN=Switch2Native Local Development') {
    throw 'Unexpected development certificate.'
}
$deviceSetup = Join-Path $package 'Switch2DeviceSetup.exe'
$restartRequired = $false
function Invoke-DeviceInstall([string]$Inf, [string]$HardwareId) {
    & $deviceSetup install $Inf $HardwareId | Out-Host
    $result = $LASTEXITCODE
    if ($result -eq 1) { $script:restartRequired = $true }
    return $result
}
function Save-Record {
    $record.RestartRequired = $script:restartRequired
    $record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $stateFile -Encoding UTF8
}
New-Item -ItemType Directory -Path $stateDirectory -Force | Out-Null
$record = @{ CertificateThumbprint=$certificate.Thumbprint; DeviceId=$null; MotionDeviceId=$null; PublishedInf=$null; BluetoothAddress=$BluetoothAddress; Status='installing' }
$record.JoyLeftAddress = $JoyLeftAddress
$record.JoyRightAddress = $JoyRightAddress
$record.JoyAddresses = $JoyAddresses
$record.JoyKinds = $JoyKinds
$record | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding UTF8
Import-Certificate -FilePath $certificateFile -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
Import-Certificate -FilePath $certificateFile -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null
foreach ($file in ($packageFiles | Where-Object { $_ -match '\.(dll|exe|cat)$' })) {
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $package $file)
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) {
        throw "Invalid package signature: $file"
    }
}
# Journal staged packages before registering nodes, including failures before a
# node has a selected driver. Recovery then owns all newly imported packages.
foreach ($inf in @('Switch2Native.inf','Switch2Sensors.inf') | Where-Object { Test-Path (Join-Path $package $_) }) {
    & pnputil.exe /add-driver (Join-Path $package $inf) | Out-Host
    if ($LASTEXITCODE -notin 0,3010) { throw "Could not stage $inf." }
    if ($LASTEXITCODE -eq 3010) { $restartRequired = $true }
    $matching = @(Get-ChildItem -LiteralPath (Join-Path $env:windir 'INF') -Filter 'oem*.inf' | Where-Object {
        (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash -eq $manifest.Files.$inf
    })
    if ($matching.Count -ne 1) { throw "Cannot identify staged $inf uniquely." }
    $field = if ($inf -eq 'Switch2Sensors.inf') { 'SensorPublishedInf' } else { 'PublishedInf' }
    $record[$field] = $matching[0].Name
    Save-Record
}
$configurationKey = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey('SOFTWARE\Switch2Native')
try {
    $proAddress = if ($BluetoothAddress) { [Convert]::ToInt64($BluetoothAddress,16) } else { [long]0 }
    $configurationKey.SetValue('BluetoothAddress',$proAddress,[Microsoft.Win32.RegistryValueKind]::QWord)
    if ($JoyLeftAddress) { $configurationKey.SetValue('JoyLeftAddress',[Convert]::ToInt64($JoyLeftAddress,16),[Microsoft.Win32.RegistryValueKind]::QWord) }
    if ($JoyRightAddress) { $configurationKey.SetValue('JoyRightAddress',[Convert]::ToInt64($JoyRightAddress,16),[Microsoft.Win32.RegistryValueKind]::QWord) }
    for ($index = 0; $index -lt $JoyAddresses.Count; $index++) {
        $configurationKey.SetValue("JoyAddress$($index + 1)",[Convert]::ToInt64($JoyAddresses[$index],16),[Microsoft.Win32.RegistryValueKind]::QWord)
        $kind = if ($JoyKinds[$index] -eq 'right') { 0x2066 } else { 0x2067 }
        $configurationKey.SetValue("JoyKind$($index + 1)",$kind,[Microsoft.Win32.RegistryValueKind]::DWord)
    }
} finally { $configurationKey.Dispose() }
$installResult = Invoke-DeviceInstall (Join-Path $package 'Switch2Native.inf') 'ROOT\SWITCH2NATIVE'
$installedDevices = @(Get-Switch2NativeDevice)
$device = if ($installedDevices.Count -eq 1) { $installedDevices[0] } else { $null }
if ($device) {
    $record.DeviceId = $device.InstanceId
    $record.PublishedInf = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction SilentlyContinue).Data
}
$record.Status = if ($installResult -le 1 -and $device) { 'installed' } else { 'failed' }
$record | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding UTF8
if ($record.Status -eq 'failed') { throw 'Installation failed. The installation record allows Uninstall-Driver.ps1 to remove this package and its certificate.' }
$motionResult = 0
# Also accept the preserved gamepad-only package when deliberately rolling back.
if (Select-String -LiteralPath (Join-Path $package 'Switch2Native.inf') -SimpleMatch 'ROOT\SWITCH2MOTION' -Quiet) {
    $motionResult = Invoke-DeviceInstall (Join-Path $package 'Switch2Native.inf') 'ROOT\SWITCH2MOTION'
    $motionDevices = @(Get-Switch2NativeDevice -HardwareId 'ROOT\SWITCH2MOTION')
    $motionDevice = if ($motionDevices.Count -eq 1) { $motionDevices[0] } else { $null }
    if ($motionDevice) { $record.MotionDeviceId = $motionDevice.InstanceId }
    $record.Status = if ($motionResult -le 1 -and $motionDevice) { 'installed' } else { 'failed' }
    $record | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding UTF8
    if ($record.Status -eq 'failed') { throw 'Companion sensor installation failed. Use Uninstall-Driver.ps1 for recovery.' }
}
if ($hasJoyCons) {
    $configurationKey = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey('SOFTWARE\Switch2Native')
    try {
        $configurationKey.SetValue('DeviceId', $record.DeviceId)
        $configurationKey.SetValue('MotionDeviceId', $record.MotionDeviceId)
    } finally { $configurationKey.Dispose() }
    $record.Status = 'installing'
    $record | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding UTF8
    foreach ($role in $joyRoles) {
        $hardwareId = "ROOT\SWITCH2$($role.ToUpperInvariant())"
        $roleInf = if ($role -like '*Sensor*') { 'Switch2Sensors.inf' } else { 'Switch2Native.inf' }
        $roleResult = Invoke-DeviceInstall (Join-Path $package $roleInf) $hardwareId
        $roleDevices = @(Get-Switch2NativeDevice -HardwareId $hardwareId)
        if ($roleDevices.Count -eq 1) {
            $roleId = $roleDevices[0].InstanceId
            $record["${role}DeviceId"] = $roleId
            if ($role -like '*Sensor*') {
                $record.SensorPublishedInf = (Get-PnpDeviceProperty -InstanceId $roleId -KeyName DEVPKEY_Device_DriverInfPath).Data
            }
            $configurationKey = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey('SOFTWARE\Switch2Native')
            try { $configurationKey.SetValue("${role}DeviceId", $roleId) } finally { $configurationKey.Dispose() }
        }
        $record | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding UTF8
        if ($roleResult -gt 1 -or $roleDevices.Count -ne 1) { throw "Could not install $role. Use Uninstall-Driver.ps1 for recovery." }
        if ($role -notin 'JoyHub','ProHub') {
            & pnputil.exe /disable-device $roleId
            if ($LASTEXITCODE -notin 0,3010) { throw "Could not disable inactive $role." }
            if ($LASTEXITCODE -eq 3010) { $restartRequired = $true }
        }
    }
    $applicationDirectory = Join-Path $env:ProgramFiles 'Switch2Native'
    if (Test-Path -LiteralPath $applicationDirectory) { throw 'The Switch2Native application folder already exists. Inspect it before replacement.' }
    New-Item -ItemType Directory -Path $applicationDirectory | Out-Null
    $record.ApplicationDirectory = $applicationDirectory
    $record | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding UTF8
    foreach ($file in 'Switch2Manager.exe','Switch2JoyCons.exe') {
        Copy-Item -LiteralPath (Join-Path $package $file) -Destination (Join-Path $applicationDirectory $file)
    }
    $serviceExecutable = Join-Path $applicationDirectory 'Switch2Manager.exe'
    New-Service -Name Switch2Manager -DisplayName 'Switch2NativeControllerDriver Manager' -BinaryPathName "`"$serviceExecutable`"" -StartupType Automatic | Out-Null
    & sc.exe failure Switch2Manager reset= 86400 actions= restart/5000/restart/10000/none/0 | Out-Null
    if ($LASTEXITCODE) { throw 'Could not configure Joy-Con manager recovery.' }
    # The service launches the tray in the signed-in user's session only when
    # a physical controller is connected; no permanent logon process is needed.
    Remove-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run' -Name Switch2JoyCons -ErrorAction SilentlyContinue
    Start-Service Switch2Manager
    $record.Status = 'installed'
    $record | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding UTF8
    Write-Output 'Installed controller manager. The tray app runs only while a controller is connected.'
}
Write-Output "Installed $($record.DeviceId), package $($record.PublishedInf). Device status: $($device.Status)."
if ($record.MotionDeviceId) { Write-Output "Installed motion companion $($record.MotionDeviceId). Device status: $($motionDevice.Status)." }
$record.Status = if ($restartRequired) { 'installed-restart-required' } else { 'installed' }
Save-Record
if ($restartRequired) { Write-Output 'Windows reports that a reboot is required.' }
Stop-Transcript | Out-Null
