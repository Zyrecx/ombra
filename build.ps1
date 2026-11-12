# Configure and build Ombra.   powershell -ExecutionPolicy Bypass -File build.ps1 [Debug|Release]
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'scripts\devenv.ps1')

# nasm assembles x264's asm (third_party/x264). Pinned and hash-checked, kept under build\tools.
$nasmVer  = '2.16.03'
$nasmDir  = Join-Path $PSScriptRoot "build\tools\nasm-$nasmVer"
$nasmHash = '3EE4782247BCB874378D02F7EAB4E294A84D3D15F3F6EE2DE2F47A46AA7226E6'
if (-not (Test-Path (Join-Path $nasmDir 'nasm.exe'))) {
    $zip = Join-Path $PSScriptRoot "build\tools\nasm-$nasmVer-win64.zip"
    New-Item -ItemType Directory -Force (Split-Path $zip) | Out-Null
    Invoke-WebRequest "https://www.nasm.us/pub/nasm/releasebuilds/$nasmVer/win64/nasm-$nasmVer-win64.zip" -OutFile $zip
    if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $nasmHash) { throw "nasm download hash mismatch" }
    Expand-Archive $zip (Join-Path $PSScriptRoot 'build\tools') -Force
    Remove-Item $zip
}
$env:PATH = "$nasmDir;$env:PATH"

# .clangd for code navigation (clangd/LSP): clangd can't find this VS 18 Build Tools install on its
# own, so hand it devenv.ps1's INCLUDE dirs as system includes. Machine-specific, gitignored.
$clangd = @('CompileFlags:', '  CompilationDatabase: build/Release', '  Add:') +
          ($env:INCLUDE -split ';' | Where-Object { $_ } | ForEach-Object { "    - `"-imsvc$($_ -replace '\\','/')`"" })
Set-Content (Join-Path $PSScriptRoot '.clangd') $clangd -Encoding ascii

$config = if ($args[0]) { $args[0] } else { 'Release' }
$dir    = Join-Path $PSScriptRoot "build\$config"
# Asan: a Release build with AddressSanitizer, in its own directory (CMakeLists.txt, OMBRA_ASAN).
$extra  = if ($config -eq 'Asan') { '-DOMBRA_ASAN=ON' } else { '-DOMBRA_ASAN=OFF' }
$type   = if ($config -eq 'Asan') { 'Release' } else { $config }

cmake -G Ninja -S $PSScriptRoot -B $dir "-DCMAKE_BUILD_TYPE=$type" $extra "-DOMBRA_SENTRY_DSN=$env:OMBRA_SENTRY_DSN"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
cmake --build $dir
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Host "`nbuilt $dir"
