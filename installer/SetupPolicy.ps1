# SPDX-License-Identifier: GPL-3.0-only
# Read-only checks shared by setup and the installer regression tests.
function Test-EmbeddedSignature([string]$Path) {
    # PowerShell collapses CERT_E_UNTRUSTEDROOT into UnknownError on Windows 11.
    # Inspect the exact WinTrust result instead of allowing all unknown errors or
    # importing a certificate merely to verify an uninstalled payload.
    if (-not ('Switch2Setup.Signatures' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Switch2Setup {
    public static class Signatures {
        [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
        struct FileInfo {
            public uint Size;
            [MarshalAs(UnmanagedType.LPWStr)] public string Path;
            public IntPtr Handle, Subject;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct TrustData {
            public uint Size;
            public IntPtr Policy, Sip;
            public uint Ui, Revocation, Choice;
            public IntPtr File;
            public uint StateAction;
            public IntPtr State, Url;
            public uint Flags, Context;
            public IntPtr Settings;
        }
        [DllImport("wintrust.dll", ExactSpelling=true)]
        static extern int WinVerifyTrust(IntPtr window, ref Guid action, ref TrustData data);
        public static int Verify(string path) {
            var file = new FileInfo { Size=(uint)Marshal.SizeOf(typeof(FileInfo)), Path=path };
            var data = new TrustData { Size=(uint)Marshal.SizeOf(typeof(TrustData)),
                Ui=2, Choice=1, StateAction=1, Flags=0x1010 };
            data.File=Marshal.AllocHGlobal(Marshal.SizeOf(typeof(FileInfo)));
            Marshal.StructureToPtr(file, data.File, false);
            var action = new Guid("00AAC56B-CD44-11D0-8CC2-00C04FC295EE");
            try { return WinVerifyTrust(new IntPtr(-1), ref action, ref data); }
            finally {
                data.StateAction=2;
                WinVerifyTrust(new IntPtr(-1), ref action, ref data);
                Marshal.DestroyStructure(data.File, typeof(FileInfo));
                Marshal.FreeHGlobal(data.File);
            }
        }
    }
}
'@
    }
    $result = [Switch2Setup.Signatures]::Verify($Path)
    return $result -eq 0 -or $result -eq -2146762487 # CERT_E_UNTRUSTEDROOT only
}

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
        if (-not (Test-EmbeddedSignature (Join-Path $Package $file)) -or $signature.SignerCertificate.Thumbprint -ne $cert.Thumbprint) {
            throw "The package signature could not be verified: $file ($($signature.Status))."
        }
    }
    return $cert
}
