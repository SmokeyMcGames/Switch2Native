# SPDX-License-Identifier: GPL-3.0-only
# Read-only checks shared by setup and the installer regression tests.
function Get-PackageVersion([string]$Path) {
    $lines = @(Select-String -LiteralPath $Path -Pattern '^DriverVer=')
    if ($lines.Count -ne 1) { throw 'Expected one DriverVer in the package INF.' }
    return [version](($lines[0].Line -split ',')[1].Trim())
}

function Get-SetupAction([version]$Native, [version]$Sensor, [version[]]$NativeInstalled,
                         [version[]]$SensorInstalled, [string]$Status) {
    if (-not $Status) { return 'install' }
    if ($Status -notin 'installed','installed-restart-required') {
        throw 'An incomplete installation needs recovery before setup can continue. Keep installation.json and the setup logs.'
    }
    if ($NativeInstalled.Count -ne 12 -or $SensorInstalled.Count -ne 5) {
        throw 'The existing virtual-device layout is incomplete. Use its recorded uninstaller before installing again.'
    }
    if (@($NativeInstalled | Where-Object { $_ -gt $Native }).Count -or
        @($SensorInstalled | Where-Object { $_ -gt $Sensor }).Count) { throw 'A newer driver is already installed. Downgrades are not supported.' }
    if ($Native -eq ($NativeInstalled | Sort-Object -Descending | Select-Object -First 1) -and
        @($SensorInstalled | Where-Object { $_ -ne $Sensor }).Count -eq 0) { return 'maintain' }
    if (@($NativeInstalled | Where-Object { $_ -ge $Native }).Count -eq 0 -and
        @($SensorInstalled | Where-Object { $_ -ge $Sensor }).Count -eq 0) { return 'update' }
    # This release only changes helpers/setup; the consumer HID/IPC contract is
    # unchanged. A future incompatible driver must use the full-update path.
    if (@($NativeInstalled | Where-Object { $_ -lt [version]'0.3.8.5' -or $_ -ge $Native }).Count -eq 0 -and
        @($SensorInstalled | Where-Object { $_ -ne $Sensor }).Count -eq 0) { return 'transport' }
    throw 'This mix of installed versions cannot be updated safely by this package. Keep the existing installation and report the setup log.'
}

function Test-SetupPackage([string]$Package) {
    $files = @('Switch2NativeDriver.dll','Switch2SensorDriver.dll','Switch2Manager.exe','Switch2JoyCons.exe',
               'Switch2DeviceSetup.exe','Switch2Native.inf','Switch2Sensors.inf','Switch2Native.cat','Switch2Sensors.cat','Switch2Native.cer')
    $manifest = Get-Content -LiteralPath (Join-Path $Package 'manifest.json') -Raw | ConvertFrom-Json
    if (@($manifest.Files.PSObject.Properties.Name).Count -ne $files.Count -or
        @($manifest.Files.PSObject.Properties.Name | Where-Object { $_ -notin $files }).Count) {
        throw 'Unexpected package contents. Only the reviewed driver payload is allowed.'
    }
    foreach ($file in $files) {
        if ($manifest.Files.$file -notmatch '^[0-9A-Fa-f]{64}$' -or
            (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Package $file)).Hash -ne $manifest.Files.$file) {
            throw "Package verification failed: $file. Download setup again from the project's release page."
        }
    }
    $cert = [Security.Cryptography.X509Certificates.X509Certificate2]::new((Join-Path $Package 'Switch2Native.cer'))
    if ($cert.Subject -ne 'CN=Switch2Native Local Development' -or $cert.Thumbprint -ne $manifest.CertificateThumbprint) {
        throw 'Unexpected package signing certificate.'
    }
    if ((Get-Date) -lt $cert.NotBefore -or (Get-Date) -gt $cert.NotAfter) {
        throw 'This development package certificate is not currently valid. Download a current build; do not change the PC clock.'
    }
    foreach ($file in ($files | Where-Object { $_ -match '\.(exe|dll|cat)$' })) {
        $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $Package $file)
        if ($signature.Status -notin 'Valid','NotTrusted' -or $signature.SignerCertificate.Thumbprint -ne $cert.Thumbprint) {
            throw "The package signature could not be verified: $file ($($signature.Status))."
        }
    }
    return $cert
}
