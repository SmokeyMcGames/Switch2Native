[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$kitRoot = 'C:\Program Files (x86)\Windows Kits\10'
& cmake -S $projectRoot -B (Join-Path $projectRoot 'build') -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE) { throw 'CMake configure failed.' }
& cmake --build (Join-Path $projectRoot 'build') --config Release
if ($LASTEXITCODE) { throw 'Build failed.' }
& ctest --test-dir (Join-Path $projectRoot 'build') -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Protocol tests failed.' }
# This is a desktop development driver. The stricter /w profile rejects reuse
# of the inbox xinputhid.sys binary (2084); see the release limitations in README.
& "$kitRoot\Tools\10.0.26100.0\x64\InfVerif.exe" (Join-Path $projectRoot 'driver\Switch2Native.inf')
if ($LASTEXITCODE) { throw 'INF verification failed.' }
& "$kitRoot\Tools\10.0.26100.0\x64\InfVerif.exe" (Join-Path $projectRoot 'driver\Switch2Sensors.inf')
if ($LASTEXITCODE) { throw 'Sensor INF verification failed.' }
Write-Output 'Build and checks passed. Run Prepare-Package.ps1 to create a locally signed package.'
