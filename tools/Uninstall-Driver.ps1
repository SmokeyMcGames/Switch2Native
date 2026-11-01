#Requires -RunAsAdministrator
[CmdletBinding()]
param([string]$LogDirectory = '')
$ErrorActionPreference = 'Stop'
if (-not $LogDirectory) { $LogDirectory = Join-Path (Split-Path $PSScriptRoot -Parent) 'captures' }
New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
Start-Transcript -Path (Join-Path $LogDirectory 'driver-uninstall.log') -Append | Out-Null
$restartRequired = $false
$stateFile = Join-Path $env:ProgramData 'Switch2Native\installation.json'
if (-not (Test-Path -LiteralPath $stateFile)) { throw 'No Switch2Native installation record exists.' }
$record = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
if ($record.CertificateThumbprint -notmatch '^[0-9A-Fa-f]{40}$') { throw 'Invalid certificate thumbprint in installation record.' }
$applicationDirectory = Join-Path $env:ProgramFiles 'Switch2Native'
if ($record.ApplicationDirectory) {
    if ([IO.Path]::GetFullPath($record.ApplicationDirectory) -ne [IO.Path]::GetFullPath($applicationDirectory)) { throw 'Unexpected application directory in installation record.' }
    $service = Get-CimInstance Win32_Service -Filter "Name='Switch2Manager'"
    if ($service) {
        $expectedService = '"' + (Join-Path $applicationDirectory 'Switch2Manager.exe') + '"'
        if ($service.PathName -ne $expectedService) { throw 'The recorded service no longer belongs to Switch2Native.' }
        Stop-Service Switch2Manager -ErrorAction Stop
        & sc.exe delete Switch2Manager
        if ($LASTEXITCODE) { throw 'Could not remove the Joy-Con manager service.' }
    }
    $trayExecutable = Join-Path $applicationDirectory 'Switch2JoyCons.exe'
    Get-Process Switch2JoyCons -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $trayExecutable } | Stop-Process
    if ($record.NotificationIntegration -and (Test-Path -LiteralPath $trayExecutable)) {
        $registration = Start-Process -FilePath $trayExecutable -ArgumentList '--unregister-notifications' `
            -WindowStyle Hidden -Wait -PassThru
        if ($registration.ExitCode) { throw 'Could not remove the controller notification registration.' }
    }
    Remove-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run' -Name Switch2JoyCons -ErrorAction SilentlyContinue
}
foreach ($target in @(
    @{ Property='ProSensorDeviceId'; HardwareId='ROOT\SWITCH2PROSENSOR' },
    @{ Property='JoySensor4DeviceId'; HardwareId='ROOT\SWITCH2JOYSENSOR4' },
    @{ Property='JoySensor3DeviceId'; HardwareId='ROOT\SWITCH2JOYSENSOR3' },
    @{ Property='JoySensor2DeviceId'; HardwareId='ROOT\SWITCH2JOYSENSOR2' },
    @{ Property='JoySensor1DeviceId'; HardwareId='ROOT\SWITCH2JOYSENSOR1' },
    @{ Property='JoyMotion4DeviceId'; HardwareId='ROOT\SWITCH2JOYMOTION4' },
    @{ Property='JoyMotion3DeviceId'; HardwareId='ROOT\SWITCH2JOYMOTION3' },
    @{ Property='JoyMotion2DeviceId'; HardwareId='ROOT\SWITCH2JOYMOTION2' },
    @{ Property='JoyMotion1DeviceId'; HardwareId='ROOT\SWITCH2JOYMOTION1' },
    @{ Property='JoyPad2DeviceId'; HardwareId='ROOT\SWITCH2JOYPAD2' },
    @{ Property='JoyPad4DeviceId'; HardwareId='ROOT\SWITCH2JOYPAD4' },
    @{ Property='JoyPad3DeviceId'; HardwareId='ROOT\SWITCH2JOYPAD3' },
    @{ Property='JoyPad1DeviceId'; HardwareId='ROOT\SWITCH2JOYPAD1' },
    @{ Property='JoyHubDeviceId'; HardwareId='ROOT\SWITCH2JOYHUB' },
    @{ Property='MotionDeviceId'; HardwareId='ROOT\SWITCH2MOTION' },
    @{ Property='DeviceId'; HardwareId='ROOT\SWITCH2NATIVE' },
    @{ Property='ProHubDeviceId'; HardwareId='ROOT\SWITCH2PROHUB' }
)) {
    $deviceId = $record.($target.Property)
    if (-not $deviceId) {
    $candidates = @(Get-PnpDevice -Class HIDClass,Sensor -ErrorAction SilentlyContinue | Where-Object {
        $_.InstanceId -like 'ROOT\*' -and
        (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_HardwareIds -ErrorAction SilentlyContinue).Data -contains $target.HardwareId
    })
    if ($candidates.Count -gt 1) { throw 'More than one matching virtual device; inspect before removal.' }
    if ($candidates.Count -eq 1) {
        $deviceId=$candidates[0].InstanceId
        $packageProperty = if ($target.Property -like '*Sensor*') { 'SensorPublishedInf' } else { 'PublishedInf' }
        $record | Add-Member -NotePropertyName $packageProperty -NotePropertyValue (Get-PnpDeviceProperty -InstanceId $deviceId -KeyName DEVPKEY_Device_DriverInfPath).Data -Force
    }
}
if ($deviceId) {
    $remainingDevice = Get-PnpDevice -InstanceId $deviceId -ErrorAction SilentlyContinue
    if (-not $remainingDevice) { continue }
    $hardwareIds=(Get-PnpDeviceProperty -InstanceId $deviceId -KeyName DEVPKEY_Device_HardwareIds -ErrorAction SilentlyContinue).Data
    if ($deviceId -notlike 'ROOT\*' -or $hardwareIds -notcontains $target.HardwareId) { throw 'Device hardware ID does not belong to Switch2Native.' }
    # Disable first so open XInput/sensor clients cannot keep a removed driver running.
    $problem = (Get-PnpDeviceProperty -InstanceId $deviceId -KeyName DEVPKEY_Device_ProblemCode -ErrorAction SilentlyContinue).Data
    $nodeFlags = (Get-PnpDeviceProperty -InstanceId $deviceId -KeyName DEVPKEY_Device_DevNodeStatus -ErrorAction SilentlyContinue).Data
    # Inactive roots can retain their registration after reboot without a live
    # devnode status. They cannot be disabled; remove the verified record directly.
    # Also avoid a second disable while DN_NEED_RESTART is set.
    if ($null -ne $nodeFlags -and $problem -ne 22 -and -not ($nodeFlags -band 0x100)) {
        & pnputil.exe /disable-device $deviceId
        if ($LASTEXITCODE -notin 0,3010) { throw 'Device disable failed; leaving installation available for recovery.' }
        if ($LASTEXITCODE -eq 3010) { $restartRequired = $true }
    }
    & pnputil.exe /remove-device $deviceId
    if ($LASTEXITCODE -notin 0,3010) { throw 'Device removal failed; leaving the package and certificate for recovery.' }
    if ($LASTEXITCODE -eq 3010) { $restartRequired = $true }
}
}
$packages = @($record) + @($record.PackageHistory | Where-Object { $_ })
$packageEntries = foreach ($packageRecord in $packages) {
    @{ Inf=$packageRecord.SensorPublishedInf; HardwareId='ROOT\SWITCH2JOYSENSOR1' }
    @{ Inf=$packageRecord.PublishedInf; HardwareId='ROOT\SWITCH2NATIVE' }
}
foreach ($packageEntry in ($packageEntries | Sort-Object { $_.Inf } -Unique)) {
    if (-not $packageEntry.Inf) { continue }
    if ($packageEntry.Inf -notmatch '^oem\d+\.inf$') { throw 'Unexpected published INF.' }
    $installedInf = Join-Path $env:windir "INF\$($packageEntry.Inf)"
    if (-not (Test-Path -LiteralPath $installedInf)) { continue }
    if ((Test-Path -LiteralPath $installedInf) -and -not (Select-String -LiteralPath $installedInf -SimpleMatch $packageEntry.HardwareId -Quiet)) {
        if ($packageEntry.Inf -in $record.PublishedInf,$record.SensorPublishedInf) {
            throw 'The active recorded INF no longer belongs to Switch2Native.'
        }
        Write-Output "Leaving recycled historical package $($packageEntry.Inf) untouched."
        continue
    }
    & pnputil.exe /delete-driver $packageEntry.Inf /uninstall
    if ($LASTEXITCODE -notin 0,3010) { throw 'Driver package removal failed; leaving the certificate for recovery.' }
    if ($LASTEXITCODE -eq 3010) { $restartRequired = $true }
}
foreach ($thumbprint in ($packages.CertificateThumbprint | Sort-Object -Unique)) {
    if ($thumbprint -notmatch '^[0-9A-Fa-f]{40}$') { throw 'Invalid certificate thumbprint in package history.' }
    foreach ($store in 'Root','TrustedPublisher') {
        $certificatePath = "Cert:\LocalMachine\$store\$thumbprint"
        if (Test-Path -LiteralPath $certificatePath) {
            if ((Get-Item -LiteralPath $certificatePath).Subject -ne 'CN=Switch2Native Local Development') {
                throw 'Recorded certificate no longer belongs to Switch2Native.'
            }
            Remove-Item -LiteralPath $certificatePath
        }
    }
}
foreach ($name in 'BluetoothAddress','JoyLeftAddress','JoyRightAddress','JoyPairingMask','MaintenanceUntil',
    'JoyHubDeviceId','ProHubDeviceId','ProSensorDeviceId','DeviceId','MotionDeviceId') {
    Remove-ItemProperty 'HKLM:\SOFTWARE\Switch2Native' -Name $name -ErrorAction SilentlyContinue
}
for ($index = 1; $index -le 4; $index++) {
    foreach ($name in "JoyPad${index}DeviceId","JoyMotion${index}DeviceId","JoySensor${index}DeviceId","JoyAddress$index","JoyKind$index") {
        Remove-ItemProperty 'HKLM:\SOFTWARE\Switch2Native' -Name $name -ErrorAction SilentlyContinue
    }
}
if ($record.ApplicationDirectory -and (Test-Path -LiteralPath $applicationDirectory)) {
    foreach ($file in 'Switch2Manager.exe','Switch2JoyCons.exe') {
        $installedFile = Join-Path $applicationDirectory $file
        if (Test-Path -LiteralPath $installedFile) { Remove-Item -LiteralPath $installedFile }
    }
    # Do not recursively remove unexpected files from the application directory.
    if (-not (Get-ChildItem -LiteralPath $applicationDirectory -Force)) { Remove-Item -LiteralPath $applicationDirectory }
}
Remove-Item -LiteralPath $stateFile
@{ RestartRequired=$restartRequired } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $LogDirectory 'uninstall-result.json') -Encoding UTF8
Write-Output 'Removed the recorded Switch2Native devices, driver package, helper apps, and development certificate.'
Stop-Transcript | Out-Null
