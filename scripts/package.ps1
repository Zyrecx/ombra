# Release packages from build\Release: out\installer\Ombra-Setup.exe (installer\ombra.iss) and
# out\installer\Ombra-Portable.zip. Build first (build.ps1). Inno Setup is pinned and hash-checked
# like build.ps1's nasm, and unpacked portably under build\tools (no install, no registry).
$ErrorActionPreference = 'Stop'
$root  = Split-Path $PSScriptRoot
$build = Join-Path $root 'build\Release'
$out   = Join-Path $root 'out\installer'

$innoVer  = '6.7.3'
$innoDir  = Join-Path $root "build\tools\inno-$innoVer"
$innoHash = '9C73C3BAE7ED48D44112A0F48E66742C00090BDB5BEF71D9D3C056C66E97B732'
$iscc     = Join-Path $innoDir 'ISCC.exe'
if (-not (Test-Path $iscc)) {
    $setup = Join-Path $root "build\tools\innosetup-$innoVer.exe"
    New-Item -ItemType Directory -Force (Split-Path $setup) | Out-Null
    $tag = 'is-' + ($innoVer -replace '\.', '_')
    Invoke-WebRequest "https://github.com/jrsoftware/issrc/releases/download/$tag/innosetup-$innoVer.exe" -OutFile $setup
    if ((Get-FileHash $setup -Algorithm SHA256).Hash -ne $innoHash) { throw "Inno Setup download hash mismatch" }
    # /PORTABLE=1: Inno's own setup copies the compiler into /DIR and writes no uninstall entry.
    $p = Start-Process $setup -Wait -PassThru -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/CURRENTUSER',
        '/PORTABLE=1', '/NOICONS', "/DIR=`"$innoDir`""
    if ($p.ExitCode -ne 0 -or -not (Test-Path $iscc)) { throw "Inno Setup unpack failed ($($p.ExitCode))" }
    Remove-Item $setup
}

foreach ($f in 'ombra.exe', 'ombra-ui.exe') {
    if (-not (Test-Path (Join-Path $build $f))) { throw "$f missing: run build.ps1 first" }
}
New-Item -ItemType Directory -Force $out | Out-Null

& $iscc /Q "/DBuildDir=$build" "/DOutputDir=$out" (Join-Path $root 'installer\ombra.iss')
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }

# The portable zip: the same files as the installer, no uninstaller (so the updater treats it as
# portable and only says when a version is out).
$stage = Join-Path $out 'portable'
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item (Join-Path $build 'ombra.exe'), (Join-Path $build 'ombra-ui.exe'), (Join-Path $root 'THIRD-PARTY-NOTICES.txt') $stage
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')
$zip = Join-Path $out 'Ombra-Portable.zip'
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive (Join-Path $stage '*') $zip
Remove-Item -Recurse -Force $stage

Get-ChildItem $out -File | ForEach-Object { '{0,-22} {1,8:N0} KB  {2}' -f $_.Name, ($_.Length / 1KB), (Get-FileHash $_.FullName).Hash }
