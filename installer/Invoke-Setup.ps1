[CmdletBinding()]
param(
    [ValidateSet('Check','Install','Uninstall')][string]$Action = 'Check',
    [Parameter(Mandatory)][string]$ResultFile,
    [switch]$TrustLocalDevelopmentCertificate
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'SetupPolicy.ps1')
$root = Split-Path $PSScriptRoot -Parent
$package = Join-Path $root 'out\driver'
$stateFile = Join-Path $env:ProgramData 'Switch2Native\installation.json'
$logs = Join-Path $env:ProgramData 'Switch2Native\logs'
$restart = $false
$transaction = $null
$locked = $false
$exitCode = 1
try {
    if (-not [Environment]::Is64BitProcess -or $env:PROCESSOR_ARCHITECTURE -ne 'AMD64' -or
        [Environment]::OSVersion.Version.Build -lt 22000) { throw 'Windows 11 on an x64 PC is required.' }
    if ($Action -ne 'Check') {
        $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
        if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Administrator access is required.' }
        $transaction = [Threading.Mutex]::new($false, 'Global\Switch2Native.Setup.Transaction')
        try { $locked = $transaction.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked = $true }
        if (-not $locked) { throw 'Another setup or uninstall is running. Wait for it to finish.' }
        New-Item -ItemType Directory -Path $logs -Force | Out-Null
    }
    if ($Action -eq 'Uninstall') {
        if (Test-Path -LiteralPath $stateFile) {
            & (Join-Path $root 'tools\Uninstall-Driver.ps1') -LogDirectory $logs
            $restart = (Get-Content (Join-Path $logs 'uninstall-result.json') -Raw | ConvertFrom-Json).RestartRequired
        }
        $message = 'Driver, service and recorded development trust removed. Controller profiles and Windows pairings were kept.'
    } else {
        $certificate = Test-SetupPackage $package
        $native = Get-PackageVersion (Join-Path $package 'Switch2Native.inf')
        $sensor = Get-PackageVersion (Join-Path $package 'Switch2Sensors.inf')
        $record = $null
        $nativeInstalled = @()
        $sensorInstalled = @()
        $pendingRoles = @()
        if (Test-Path -LiteralPath $stateFile) {
            $record = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
            foreach ($role in 'Native','Motion','ProHub','JoyHub','JoyPad1','JoyPad2','JoyPad3','JoyPad4',
                'JoyMotion1','JoyMotion2','JoyMotion3','JoyMotion4','ProSensor','JoySensor1','JoySensor2','JoySensor3','JoySensor4') {
                $field = if ($role -eq 'Native') { 'DeviceId' } else { "${role}DeviceId" }
                $id = $record.$field
                $hardware = "ROOT\SWITCH2$($role.ToUpperInvariant())"
                if (-not $id -or $id -notlike 'ROOT\*') { throw "Invalid recorded $role identity." }
                $properties = @{}
                Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_HardwareIds,DEVPKEY_Device_DriverVersion,DEVPKEY_Device_IsRebootRequired |
                    ForEach-Object { $properties[$_.KeyName] = $_.Data }
                if ($properties.DEVPKEY_Device_HardwareIds -notcontains $hardware) {
                    throw "The recorded $role device is missing or has changed. Keep the installation record for recovery."
                }
                $version = [version]$properties.DEVPKEY_Device_DriverVersion
                if ($role -like '*Sensor*') { $sensorInstalled += $version } else { $nativeInstalled += $version }
                if ($properties.DEVPKEY_Device_IsRebootRequired) { $pendingRoles += $role }
            }
            $service = Get-CimInstance Win32_Service -Filter "Name='Switch2Manager'"
            $expectedDirectory = Join-Path $env:ProgramFiles 'Switch2Native'
            if ($record.ApplicationDirectory -ne $expectedDirectory -or -not $service -or
                $service.PathName -ne ('"' + (Join-Path $expectedDirectory 'Switch2Manager.exe') + '"')) {
                throw 'The installed service ownership does not match. Setup will not replace an unrelated service.'
            }
        }
        $operation = Get-SetupAction $native $sensor $nativeInstalled $sensorInstalled $record.Status
        $restart = if ($operation -eq 'transport') {
            @($pendingRoles | Where-Object { $_ -in 'ProHub','JoyHub' }).Count -gt 0
        } else { $pendingRoles.Count -gt 0 }
        if ($restart -and $operation -ne 'maintain') { throw 'Windows has a pending driver restart. Save your work, restart Windows, then run setup again.' }
        $message = "Ready: $operation. Driver $native. Controller profiles and Windows pairings are preserved."
        if ($Action -eq 'Install') {
            if (-not $TrustLocalDevelopmentCertificate) { throw 'Explicit consent to the development certificate is required.' }
            switch ($operation) {
                'install' {
                    try {
                        & (Join-Path $root 'tools\Install-Driver.ps1') -TrustLocalDevelopmentCertificate -LogDirectory $logs
                    } catch {
                        $installError = $_.Exception.Message
                        # Only roll back a fresh transaction; never uninstall a pre-existing driver on update failure.
                        if (Test-Path -LiteralPath $stateFile) {
                            try { & (Join-Path $root 'tools\Uninstall-Driver.ps1') -LogDirectory $logs }
                            catch { throw "$installError Recovery also needs attention: $($_.Exception.Message)" }
                        }
                        throw "$installError The new installation was rolled back."
                    }
                }
                'update' { & (Join-Path $root 'tools\Update-Driver.ps1') -TrustLocalDevelopmentCertificate -LogDirectory $logs }
                'transport' { & (Join-Path $root 'tools\Update-Driver.ps1') -TrustLocalDevelopmentCertificate -TransportOnly -LogDirectory $logs }
                'maintain' {
                    # Re-running setup restores its management files/Installed apps
                    # entry. It does not force a same-version driver replacement.
                    Write-Output 'This driver version is already installed; refreshing setup and documentation only.'
                }
            }
            $record = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
            Set-Service -Name Switch2Manager -DisplayName 'Switch2NativeControllerDriver Manager'
            $restart = $restart -or $record.RestartRequired
            $message = 'Ready to connect. Pair in Windows Bluetooth settings, or connect the Pro Controller by USB.'
        }
    }
    $exitCode = if ($restart) { 3010 } else { 0 }
} catch {
    $message = $_.Exception.Message
    if ($Action -ne 'Check') {
        $message | Add-Content -LiteralPath (Join-Path $logs 'setup-errors.log') -Encoding UTF8 -ErrorAction SilentlyContinue
        # Preserve a trusted recovery kit outside Inno's temporary directory. It
        # remains available even when setup never reaches its file-copy phase.
        try {
            $recovery = Join-Path $env:ProgramFiles ('Switch2Native Setup Recovery\' + [guid]::NewGuid().ToString('N'))
            New-Item -ItemType Directory -Path $recovery -Force | Out-Null
            foreach ($folder in 'installer','tools','out\driver') {
                New-Item -ItemType Directory -Path (Join-Path $recovery $folder) -Force | Out-Null
                Get-ChildItem -LiteralPath (Join-Path $root $folder) -File | ForEach-Object {
                    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $recovery $folder)
                }
            }
            $message += " Recovery scripts: $recovery\tools. Logs: $logs."
        } catch { $message += ' See the existing installation record and Windows SetupAPI log for recovery.' }
    }
} finally {
    if ($locked) { $transaction.ReleaseMutex() }
    if ($transaction) { $transaction.Dispose() }
    $message = $message -replace '[\r\n]+',' '
    @('[Setup]', "Message=$message", "RestartRequired=$restart", "ExitCode=$exitCode") |
        Set-Content -LiteralPath $ResultFile -Encoding Unicode
}
exit $exitCode
