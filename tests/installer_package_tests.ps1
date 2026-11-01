$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'installer\SetupPolicy.ps1')
$package = Join-Path $root 'out\driver'
$certificate = Test-SetupPackage $package
if (Test-Path "Cert:\CurrentUser\My\$($certificate.Thumbprint)") { throw 'The temporary signing key was not removed.' }
foreach ($store in 'Root','TrustedPublisher') {
    if (Test-Path "Cert:\LocalMachine\$store\$($certificate.Thumbprint)") { throw 'Packaging unexpectedly installed machine trust.' }
}
$scratch = Join-Path $root ('out\package-test-' + [guid]::NewGuid().ToString('N'))
Copy-Item -LiteralPath $package -Destination $scratch -Recurse
$binary = Join-Path $scratch 'Switch2NativeDriver.dll'
$bytes = [IO.File]::ReadAllBytes($binary)
$bytes[1024] = $bytes[1024] -bxor 1
[IO.File]::WriteAllBytes($binary, $bytes)
if (Test-EmbeddedSignature $binary) { throw 'A damaged PE signature was accepted.' }
$rejected = $false
try { Test-SetupPackage $scratch | Out-Null } catch { $rejected = $true }
if (-not $rejected) { throw 'A changed payload passed its manifest check.' }
# Even a recalculated manifest must not make the damaged signature acceptable.
$manifestPath = Join-Path $scratch 'manifest.json'
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
$manifest.Files.'Switch2NativeDriver.dll' = (Get-FileHash $binary).Hash
$manifest | ConvertTo-Json -Depth 3 | Set-Content $manifestPath -Encoding UTF8
$rejected = $false
try { Test-SetupPackage $scratch | Out-Null } catch { $rejected = $true }
if (-not $rejected) { throw 'A tampered binary with a rewritten manifest was accepted.' }
Write-Output 'Package signature, tamper rejection and no-trust/private-key checks passed.'
