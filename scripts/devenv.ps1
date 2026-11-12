# Dot-source this to get a working MSVC x64 environment: . scripts\devenv.ps1
#
# We compose INCLUDE/LIB/PATH ourselves rather than calling vcvars64.bat or VsDevCmd.bat. The
# VS 18 Build Tools layout ships no vcvars64.bat, and VsDevCmd.bat resolves its own helpers
# relative to the current directory, which fails in some shells.

$ErrorActionPreference = 'Stop'

# Pick up PATH edits made by installers since this shell started.
$env:PATH = ([Environment]::GetEnvironmentVariable('Path','Machine') + ';' +
             [Environment]::GetEnvironmentVariable('Path','User') + ';' + $env:PATH)

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { $vswhere = Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe' }
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found - install Visual Studio Build Tools' }

$vs = & $vswhere -latest -products * -property installationPath
if (-not $vs) { throw 'vswhere found no Visual Studio installation' }

# Newest toolset that actually has a compiler - VS leaves stub version folders behind.
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory -EA Stop |
        Sort-Object Name -Descending |
        Where-Object { Test-Path (Join-Path $_.FullName 'bin\HostX64\x64\cl.exe') } |
        Select-Object -First 1
if (-not $msvc) { throw "no x64 cl.exe under $vs\VC\Tools\MSVC" }

$kits = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots' -EA 0).KitsRoot10
if (-not $kits) { $kits = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -EA 0).KitsRoot10 }
if (-not $kits) { throw 'Windows 10/11 SDK not found in the registry' }
$sdk = Get-ChildItem (Join-Path $kits 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1

$inc = $sdk.FullName
$lib = Join-Path $kits "Lib\$($sdk.Name)"

$env:PATH = "$(Join-Path $msvc.FullName 'bin\HostX64\x64');$(Join-Path $kits "bin\$($sdk.Name)\x64");$env:PATH"
$env:INCLUDE = @(
    (Join-Path $msvc.FullName 'include')
    (Join-Path $inc 'ucrt'), (Join-Path $inc 'um'), (Join-Path $inc 'shared')
    (Join-Path $inc 'winrt'), (Join-Path $inc 'cppwinrt')
) -join ';'
$env:LIB = @(
    (Join-Path $msvc.FullName 'lib\x64')
    (Join-Path $lib 'ucrt\x64'), (Join-Path $lib 'um\x64')
) -join ';'

Write-Host "msvc $($msvc.Name)   sdk $($sdk.Name)"
