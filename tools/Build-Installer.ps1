[CmdletBinding()]
param(
    [string]$IsccPath = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    [switch]$PreviewUiOnly
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'installer\SetupPolicy.ps1')
$package = Join-Path $root 'out\driver'
$certificate = Test-SetupPackage $package
if (-not (Test-Path -LiteralPath $IsccPath)) { throw 'Install Inno Setup 6.7.4 or supply -IsccPath. It is a build tool only, not an end-user requirement.' }
$IsccPath = (Resolve-Path -LiteralPath $IsccPath).Path
# ISCC's PE version resource is 0.0.0.0; pin the reviewed official binary instead.
$compilerVersion = 'Inno Setup 6.7.4'
if ((Get-FileHash -LiteralPath $IsccPath).Hash -ne 'F02FA0E5F237B47D7ED1EDAE9B991866EC94FE02AF61EB6A73E94F92FE8E0604') {
    throw 'Use the reviewed Inno Setup 6.7.4 toolchain (tools/Get-InnoCompiler.ps1).'
}
$version = (Get-PackageVersion (Join-Path $package 'Switch2Native.inf')).ToString()
Push-Location $root
try {
    $sourceCommit = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE) { throw 'A Git checkout is required to include corresponding source.' }
    $dirty = @(& git status --porcelain --untracked-files=normal)
    if ($dirty.Count) { throw 'Commit source changes before packaging. Setup includes an exact archive of that commit.' }
    $stage = Join-Path $root ('out\setup-stage-' + [guid]::NewGuid().ToString('N'))
    $output = Join-Path $root 'out\installer'
    foreach ($folder in $stage,$output,(Join-Path $stage 'out\driver'),(Join-Path $stage 'tools'),
        (Join-Path $stage 'installer'),(Join-Path $stage 'licenses'),(Join-Path $stage 'source')) {
        New-Item -ItemType Directory -Path $folder -Force | Out-Null
    }
    $manifest = Get-Content (Join-Path $package 'manifest.json') -Raw | ConvertFrom-Json
    if ($manifest.SourceCommit -ne $sourceCommit) { throw 'Prepare this package from the current clean commit before building the installer.' }
    foreach ($file in @($manifest.Files.PSObject.Properties.Name) + 'manifest.json') {
        if ($file -notmatch '^[A-Za-z0-9_.-]+$') { throw 'Invalid manifest filename.' }
        Copy-Item -LiteralPath (Join-Path $package $file) -Destination (Join-Path $stage 'out\driver')
    }
    foreach ($file in 'Install-Driver.ps1','Update-Driver.ps1','Uninstall-Driver.ps1') {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination (Join-Path $stage 'tools')
    }
    foreach ($file in 'Invoke-Setup.ps1','SetupPolicy.ps1') {
        Copy-Item -LiteralPath (Join-Path $root "installer\$file") -Destination (Join-Path $stage 'installer')
    }
    foreach ($file in 'LICENSE','COPYRIGHT.md','THIRD_PARTY_NOTICES.md','PRIVACY.md') {
        Copy-Item -LiteralPath (Join-Path $root $file) -Destination (Join-Path $stage 'licenses')
    }
    Copy-Item -LiteralPath (Join-Path $root 'installer\INNO-LICENSE.txt') -Destination (Join-Path $stage 'licenses')
    Copy-Item -LiteralPath (Join-Path $root 'assets\controller.ico') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $root 'installer\Getting started.html') -Destination $stage
    $sourceName = "Switch2NativeControllerDriver-$version-source.zip"
    & git archive --format=zip "--output=$(Join-Path $stage "source\$sourceName")" HEAD
    if ($LASTEXITCODE) { throw 'Source archive failed.' }
    @{ Version=$version; SourceCommit=$sourceCommit; Signing='Local development; not Microsoft certified';
       CertificateThumbprint=$certificate.Thumbprint; CertificateExpiry=$certificate.NotAfter.ToUniversalTime().ToString('o');
       SourceArchive="source/$sourceName"; Compiler=$compilerVersion.ToString() } |
        ConvertTo-Json | Set-Content (Join-Path $stage 'build-info.json') -Encoding UTF8
    @('#define PackageVersion "' + $version + '"',
      '#define CertificateThumbprint "' + $certificate.Thumbprint + '"',
      '#define CertificateExpiry "' + $certificate.NotAfter.ToString('yyyy-MM-dd') + '"') |
        Set-Content (Join-Path $stage 'package.iss') -Encoding UTF8
    $arguments = @("/DStageDir=$stage", "/DOutputDir=$output")
    if ($PreviewUiOnly) { $arguments += '/DPreviewUiOnly=1' }
    & $IsccPath @arguments (Join-Path $root 'installer\Setup.iss')
    if ($LASTEXITCODE) { throw 'Installer compilation failed.' }
    if (-not $PreviewUiOnly) {
        Copy-Item -LiteralPath (Join-Path $stage "source\$sourceName") -Destination $output -Force
        Copy-Item -LiteralPath (Join-Path $stage 'build-info.json') -Destination $output -Force
        $names = @("Switch2NativeControllerDriver-$version-Preview-Setup-x64.exe",$sourceName,'build-info.json')
        $hashes = foreach ($name in $names) { ((Get-FileHash (Join-Path $output $name)).Hash.ToLowerInvariant() + '  ' + $name) }
        $hashes | Set-Content (Join-Path $output 'SHA256SUMS.txt') -Encoding ASCII
    }
    Write-Output "Created $output from commit $sourceCommit. No driver installation or machine trust changed."
} finally { Pop-Location }
