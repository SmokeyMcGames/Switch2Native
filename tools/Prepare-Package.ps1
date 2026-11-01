[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$package = Join-Path $projectRoot 'out\driver'
$kitRoot = 'C:\Program Files (x86)\Windows Kits\10'
$signTool = "$kitRoot\bin\10.0.26100.0\x64\signtool.exe"
$binaries = @('Switch2NativeDriver.dll','Switch2Manager.exe','Switch2JoyCons.exe','Switch2DeviceSetup.exe')
$infs = @('Switch2Native.inf')
$catalogs = @('Switch2Native.cat')
if (Test-Path -LiteralPath (Join-Path $package 'Switch2Sensors.inf')) {
    $binaries += 'Switch2SensorDriver.dll'
    $infs += 'Switch2Sensors.inf'
    $catalogs += 'Switch2Sensors.cat'
}
foreach ($file in ($binaries + $infs)) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $file))) { throw "Missing $file. Run Build.ps1 first." }
}
$certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Switch2Native Local Development' `
    -CertStoreLocation 'Cert:\CurrentUser\My' -HashAlgorithm SHA256 -KeyAlgorithm RSA -KeyLength 3072 `
    -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddMonths(3)
try {
    Export-Certificate -Cert $certificate -FilePath (Join-Path $package 'Switch2Native.cer') | Out-Null
    foreach ($file in $binaries) {
        & $signTool sign /fd SHA256 /s My /sha1 $certificate.Thumbprint (Join-Path $package $file)
        if ($LASTEXITCODE) { throw "Signing failed: $file" }
    }
    & "$kitRoot\bin\10.0.26100.0\x86\Inf2Cat.exe" "/driver:$package" /os:10_GE_X64 /uselocaltime
    if ($LASTEXITCODE) { throw 'Catalog generation failed.' }
    foreach ($catalog in $catalogs) {
        & $signTool sign /fd SHA256 /s My /sha1 $certificate.Thumbprint (Join-Path $package $catalog)
        if ($LASTEXITCODE) { throw "Catalog signing failed: $catalog" }
    }
    $hashes = @{}
    foreach ($file in ($binaries + $infs + $catalogs + 'Switch2Native.cer')) {
        $hashes[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $package $file)).Hash
    }
    $sourceCommit = $null
    Push-Location $projectRoot
    try {
        if (-not @(& git status --porcelain --untracked-files=normal).Count) { $sourceCommit = (& git rev-parse HEAD).Trim() }
    } finally { Pop-Location }
    @{ CertificateThumbprint=$certificate.Thumbprint; Files=$hashes; SourceCommit=$sourceCommit } | ConvertTo-Json -Depth 3 |
        Set-Content -LiteralPath (Join-Path $package 'manifest.json') -Encoding UTF8
    Write-Output "Prepared $package. Certificate: $($certificate.Thumbprint). No system trust or driver installation has changed."
} finally {
    # The package can be verified with the exported public certificate; no reusable signing key remains.
    Remove-Item -LiteralPath "Cert:\CurrentUser\My\$($certificate.Thumbprint)" -DeleteKey
}
