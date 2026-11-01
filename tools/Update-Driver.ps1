#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [Parameter(Mandatory)][switch]$TrustLocalDevelopmentCertificate,
    [switch]$TransportOnly,
    [switch]$Resume,
    [string]$LogDirectory = ''
)

$ErrorActionPreference = 'Stop'
if (-not $TrustLocalDevelopmentCertificate) { throw 'Local certificate trust must be explicitly selected.' }
$projectRoot = Split-Path $PSScriptRoot -Parent
$package = Join-Path $projectRoot 'out\driver'
$stateDirectory = Join-Path $env:ProgramData 'Switch2Native'
$stateFile = Join-Path $stateDirectory 'installation.json'
$applicationDirectory = Join-Path $env:ProgramFiles 'Switch2Native'
$record = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
if ($Resume -and $record.Status -ne 'update-failed') { throw '-Resume requires a recorded failed update.' }
$manifest = Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw | ConvertFrom-Json
$certificateFile = Join-Path $package 'Switch2Native.cer'
$certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($certificateFile)
if ($certificate.Thumbprint -ne $manifest.CertificateThumbprint -or
    $certificate.Subject -ne 'CN=Switch2Native Local Development') { throw 'Unexpected development certificate.' }
if ([IO.Path]::GetFullPath($record.ApplicationDirectory) -ne [IO.Path]::GetFullPath($applicationDirectory)) {
    throw 'Unexpected application directory in installation record.'
}
$service = Get-CimInstance Win32_Service -Filter "Name='Switch2Manager'"
if (-not $service -or $service.PathName -ne ('"' + (Join-Path $applicationDirectory 'Switch2Manager.exe') + '"')) {
    throw 'The recorded service no longer belongs to Switch2Native.'
}
$files = @('Switch2NativeDriver.dll','Switch2Native.inf','Switch2Native.cat','Switch2Native.cer',
    'Switch2SensorDriver.dll','Switch2Sensors.inf','Switch2Sensors.cat','Switch2Manager.exe','Switch2JoyCons.exe')
foreach ($file in $files) {
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $package $file)).Hash -ne $manifest.Files.$file) {
        throw "Package changed after signing: $file"
    }
}
function Get-Property([string]$DeviceId, [string]$Name) {
    (Get-PnpDeviceProperty -InstanceId $DeviceId -KeyName $Name -ErrorAction SilentlyContinue).Data
}
function Save-Record {
    $temporary = "$stateFile.tmp"
    $record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -LiteralPath $temporary -Destination $stateFile -Force
}
function Set-RecordValue([string]$Name, $Value) {
    $record | Add-Member -NotePropertyName $Name -NotePropertyValue $Value -Force
}
function Invoke-Pnp([string[]]$Arguments) {
    & pnputil.exe @Arguments | Out-Host
    $result = $LASTEXITCODE
    if ($result -eq 3010) { $script:restartRequired = $true }
    elseif ($result -ne 0) { throw "PnPUtil failed ($result): $($Arguments -join ' ')" }
}
function Restore-Device($Target) {
    if ((Get-Property $Target.Id 'DEVPKEY_Device_ProblemCode') -ne 22) { return }
    if (Get-Property $Target.Id 'DEVPKEY_Device_IsRebootRequired') {
        $script:restartRequired = $true
        Write-Output "Windows deferred enabling $($Target.Id) until restart."
        return
    }
    Invoke-Pnp @('/enable-device', $Target.Id)
}
function Find-PublishedInf([string]$FileName, [string]$HardwareId) {
    $hash = $manifest.Files.$FileName
    $matching = @(Get-ChildItem -LiteralPath (Join-Path $env:windir 'INF') -Filter 'oem*.inf' | Where-Object {
        (Select-String -LiteralPath $_.FullName -SimpleMatch $HardwareId -Quiet) -and
        (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash -eq $hash
    })
    if ($matching.Count -ne 1) { throw "Cannot identify the staged $FileName package uniquely." }
    $matching[0].Name
}
if ($TransportOnly) {
    # Existing consumer HID reports and IPC commands must stay compatible. New
    # optional hub reports/helper commands can be added without replacing consumers.
    # Consumer fixes or incompatible report/layout changes need a full update.
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Switch2DriverUpdate {
    [DllImport("newdev.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr window,
        string hardwareId, string inf, uint flags, [MarshalAs(UnmanagedType.Bool)] out bool reboot);
}
'@
}
# Readers close before the HID collections they consume. Existing root instance
# IDs, Windows pairing and tray preferences survive this in-place update.
$roles = @('ProSensor','JoySensor4','JoySensor3','JoySensor2','JoySensor1',
    'Motion','JoyMotion4','JoyMotion3','JoyMotion2','JoyMotion1',
    'Native','JoyPad4','JoyPad3','JoyPad2','JoyPad1','ProHub','JoyHub')
$targets = foreach ($role in $roles) {
    $property = if ($role -eq 'Native') { 'DeviceId' } else { "${role}DeviceId" }
    $id = $record.$property
    $hardware = "ROOT\SWITCH2$($role.ToUpperInvariant())"
    if (-not $id -or $id -notlike 'ROOT\*' -or
        (Get-Property $id 'DEVPKEY_Device_HardwareIds') -notcontains $hardware) {
        throw "Unexpected or missing $role device. Use the full installer only for a changed device layout."
    }
    $selected = -not $TransportOnly -or $role -in 'ProHub','JoyHub'
    if ($selected -and -not $Resume -and (Get-Property $id 'DEVPKEY_Device_IsRebootRequired')) {
        throw "Windows already has a pending restart for $id. Finish that restart before updating."
    }
    $infName = if ($role -like '*Sensor*') { 'Switch2Sensors.inf' } else { 'Switch2Native.inf' }
    $versionLine = (Select-String -LiteralPath (Join-Path $package $infName) -Pattern '^DriverVer=').Line
    $version = [version](($versionLine -split ',')[1])
    $installedVersion = [version](Get-Property $id 'DEVPKEY_Device_DriverVersion')
    if ($selected -and ($version -lt $installedVersion -or ($version -eq $installedVersion -and -not $Resume))) {
        throw "Update version $version must be newer than installed $installedVersion."
    }
    if ($TransportOnly -and $installedVersion -lt [version]'0.3.8.5') {
        throw 'Transport-only updates require the maintenance-aware 0.3.8.5 consumer drivers first.'
    }
    $flags = Get-Property $id 'DEVPKEY_Device_DevNodeStatus'
    [pscustomobject]@{ Role=$role; Id=$id; HardwareId=$hardware; Selected=$selected;
        NeedsUpdate=($selected -and $version -gt $installedVersion);
        Version=$(if ($selected) { $version.ToString() } else { $installedVersion.ToString() });
        Started=($null -ne $flags -and ($flags -band 8) -ne 0) }
}
if (-not $LogDirectory) { $LogDirectory = Join-Path $projectRoot 'captures' }
New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
Start-Transcript -Path (Join-Path $LogDirectory 'driver-update.log') -Append | Out-Null
$restartRequired = $false
$serviceStopped = $false
$backup = Join-Path $stateDirectory ('updates\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backup | Out-Null
Copy-Item -LiteralPath $stateFile -Destination $backup
foreach ($file in 'Switch2Manager.exe','Switch2JoyCons.exe') {
    Copy-Item -LiteralPath (Join-Path $applicationDirectory $file) -Destination $backup
}
try {
    $history = @($record.PackageHistory | Where-Object { $_ })
    $history += [pscustomobject]@{ CertificateThumbprint=$record.CertificateThumbprint;
        PublishedInf=$record.PublishedInf; SensorPublishedInf=$record.SensorPublishedInf }
    # Resuming an interrupted update must not add the same ownership record again.
    $history = @($history | Group-Object { "$($_.CertificateThumbprint)|$($_.PublishedInf)|$($_.SensorPublishedInf)" } |
        ForEach-Object { $_.Group[0] })
    Set-RecordValue PackageHistory $history
    Set-RecordValue Status 'updating'
    Set-RecordValue CertificateThumbprint $certificate.Thumbprint
    Set-RecordValue UpdateBackup $backup
    Save-Record
    Import-Certificate -FilePath $certificateFile -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
    Import-Certificate -FilePath $certificateFile -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null
    foreach ($file in 'Switch2NativeDriver.dll','Switch2SensorDriver.dll','Switch2Manager.exe','Switch2JoyCons.exe','Switch2Native.cat','Switch2Sensors.cat') {
        $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $package $file)
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) {
            throw "Invalid package signature: $file"
        }
    }
    # Stage and journal both packages before touching any active device.
    Invoke-Pnp @('/add-driver', (Join-Path $package 'Switch2Native.inf'))
    Set-RecordValue PublishedInf (Find-PublishedInf 'Switch2Native.inf' 'ROOT\SWITCH2NATIVE')
    Save-Record
    if (-not $TransportOnly) {
        Invoke-Pnp @('/add-driver', (Join-Path $package 'Switch2Sensors.inf'))
        Set-RecordValue SensorPublishedInf (Find-PublishedInf 'Switch2Sensors.inf' 'ROOT\SWITCH2PROSENSOR')
        Save-Record
    }
    $configuration = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey('SOFTWARE\Switch2Native')
    try { $configuration.SetValue('MaintenanceUntil', [DateTime]::UtcNow.AddSeconds(120).ToFileTimeUtc(), [Microsoft.Win32.RegistryValueKind]::QWord) }
    finally { $configuration.Dispose() }
    # Readers check at most every 250ms; an idle HID read has a two-second bound.
    Start-Sleep -Seconds 3
    $controllerService = Get-Service Switch2Manager
    $controllerService.Stop()
    $controllerService.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
    $serviceStopped = $true
    $trayExecutable = Join-Path $applicationDirectory 'Switch2JoyCons.exe'
    Get-Process Switch2JoyCons -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $trayExecutable } | Stop-Process
    if ($TransportOnly) {
        foreach ($target in ($targets | Where-Object NeedsUpdate)) {
            $reboot = $false
            if (-not [Switch2DriverUpdate]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero,
                $target.HardwareId, (Join-Path $package 'Switch2Native.inf'), 0, [ref]$reboot)) {
                throw "Targeted update failed for $($target.Id): $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
            }
            if ($reboot) { $restartRequired = $true }
            Write-Output "Updated $($target.Role); restart required: $reboot"
        }
    } else {
        # Updating in place avoids the redundant disable/remove/create cycle.
        foreach ($file in 'Switch2Native.inf','Switch2Sensors.inf') {
            $sensor = $file -eq 'Switch2Sensors.inf'
            if ($targets | Where-Object { $_.NeedsUpdate -and (($_.Role -like '*Sensor*') -eq $sensor) }) {
                Invoke-Pnp @('/add-driver', (Join-Path $package $file), '/install')
            }
        }
    }
    foreach ($file in 'Switch2Manager.exe','Switch2JoyCons.exe') {
        Copy-Item -LiteralPath (Join-Path $package $file) -Destination (Join-Path $applicationDirectory $file) -Force
    }
    $restore = @($targets | Where-Object { $_.Started })
    [array]::Reverse($restore)
    foreach ($target in $restore) {
        Restore-Device $target
    }
    foreach ($target in $targets) {
        if ((Get-Property $target.Id 'DEVPKEY_Device_DriverVersion') -ne $target.Version) {
            throw "Windows did not select the new package for $($target.Id)."
        }
        if ($target.Selected -and (Get-Property $target.Id 'DEVPKEY_Device_IsRebootRequired')) { $restartRequired = $true }
    }
    # No /force or /uninstall: keep a superseded package and its trust if Windows
    # still uses it. The uninstall record owns every retained package/certificate.
    $retained = @()
    foreach ($old in $history) {
        $inUse = $false
        foreach ($entry in @(@{Name=$old.PublishedInf; Id='ROOT\SWITCH2NATIVE'; Field='PublishedInf'},
                            @{Name=$old.SensorPublishedInf; Id='ROOT\SWITCH2PROSENSOR'; Field='SensorPublishedInf'})) {
            if (-not $entry.Name) { continue }
            if ($entry.Name -in $record.PublishedInf,$record.SensorPublishedInf) { $inUse = $true; continue }
            if ($entry.Name -notmatch '^oem\d+\.inf$') { throw 'Invalid prior package name.' }
            $path = Join-Path $env:windir "INF\$($entry.Name)"
            if (Test-Path -LiteralPath $path) {
                if (-not (Select-String -LiteralPath $path -SimpleMatch $entry.Id -Quiet)) {
                    # Windows reuses oem*.inf names after deletion. An old journal
                    # entry grants no ownership of the replacement package.
                    Write-Output "Skipping recycled prior package name $($entry.Name); replacement left untouched."
                    $old.PSObject.Properties[$entry.Field].Value = $null
                    continue
                }
                & pnputil.exe /delete-driver $entry.Name | Out-Host
                if ($LASTEXITCODE -eq 3010) { $restartRequired = $true }
                if ($LASTEXITCODE -ne 0) { $inUse = $true }
            }
        }
        if ($inUse -or $restartRequired) { $retained += $old; continue }
        if ($old.CertificateThumbprint -notmatch '^[0-9A-Fa-f]{40}$') { throw 'Invalid prior certificate.' }
        if ($old.CertificateThumbprint -eq $certificate.Thumbprint) { continue }
        foreach ($store in 'Root','TrustedPublisher') {
            $path = "Cert:\LocalMachine\$store\$($old.CertificateThumbprint)"
            if (Test-Path -LiteralPath $path) {
                if ((Get-Item -LiteralPath $path).Subject -ne 'CN=Switch2Native Local Development') { throw 'Prior certificate ownership changed.' }
                Remove-Item -LiteralPath $path
            }
        }
    }
    Set-RecordValue PackageHistory $retained
    Set-RecordValue RestartRequired $restartRequired
    Set-RecordValue UpdateScope $(if ($TransportOnly) { 'transport' } else { 'full' })
    Set-RecordValue DeviceVersions @($targets | Select-Object Role,Id,Version)
    $record.PSObject.Properties.Remove('UpdateError')
    Set-RecordValue Status $(if ($restartRequired) { 'installed-restart-required' } else { 'installed' })
    Save-Record
    Write-Output "In-place update complete. Device identities and pairings preserved. Restart required: $restartRequired"
} catch {
    Set-RecordValue Status 'update-failed'
    Set-RecordValue UpdateError $_.Exception.Message
    Save-Record
    throw
} finally {
    Remove-ItemProperty -Path 'HKLM:\SOFTWARE\Switch2Native' -Name MaintenanceUntil -ErrorAction SilentlyContinue
    if ($serviceStopped) {
        # Also recover the previously active nodes if an update failed midway.
        $restore = @($targets | Where-Object { $_.Started })
        [array]::Reverse($restore)
        foreach ($target in $restore) {
            Restore-Device $target
        }
        Start-Service Switch2Manager
    }
    Stop-Transcript | Out-Null
}
