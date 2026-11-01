[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$directory = Join-Path $root '.cache\inno-6.7.4'
$compiler = Join-Path $directory 'ISCC.exe'
if (-not (Test-Path -LiteralPath $compiler)) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $download = Join-Path $directory 'inno-bootstrap.exe'
    Invoke-WebRequest 'https://github.com/jrsoftware/issrc/releases/download/is-6_7_4/innosetup-6.7.4.exe' -OutFile $download
    if ((Get-FileHash -LiteralPath $download).Hash -ne 'A549929319F9120716677C685535CA4671E4B937B5FDC87E2585F32D8EB30E8C') {
        throw 'Inno Setup download hash did not match the pinned official release.'
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $download
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notlike 'CN=Pyrsys B.V.,*') {
        throw 'Inno Setup publisher verification failed.'
    }
    $process = Start-Process -FilePath $download -WindowStyle Hidden -Wait -PassThru -ArgumentList @(
        '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/CURRENTUSER',('/DIR="' + $directory + '"'))
    if ($process.ExitCode) { throw "Inno Setup tool installation failed: $($process.ExitCode)" }
}
$compilerSignature = Get-AuthenticodeSignature -LiteralPath $compiler
if ($compilerSignature.Status -ne 'Valid' -or $compilerSignature.SignerCertificate.Subject -notlike 'CN=Pyrsys B.V.,*') {
    throw 'Installed Inno compiler publisher verification failed.'
}
Write-Output $compiler
