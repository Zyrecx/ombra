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

# A running engine locks its ombra.exe and the link fails (LNK1104). Stop the one that runs from
# this build (cleanly, so it logs no unclean exit) and start it again afterwards, even on failure.
$exe = Join-Path $dir 'ombra.exe'
$restart = $false
if (Get-Process ombra -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) {
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'scripts\stop-engine.ps1') -Path $exe
    if ($LASTEXITCODE -eq 0) { $restart = $true; Write-Host "stopped the engine running from $exe; it restarts after the build" }
    elseif ($LASTEXITCODE -ne 2) { throw "could not stop the engine: $out" }
}
try {
    cmake -G Ninja -S $PSScriptRoot -B $dir "-DCMAKE_BUILD_TYPE=$type" $extra "-DOMBRA_SENTRY_DSN=$env:OMBRA_SENTRY_DSN"
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
    cmake --build $dir
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
    Write-Host "`nbuilt $dir"
} finally {
    if ($restart) {
        # The real engine, not an isolated one: drop any OMBRA_HOME this shell carries.
        $home_ = $env:OMBRA_HOME
        Remove-Item Env:OMBRA_HOME -ErrorAction SilentlyContinue
        Start-Process $exe -WindowStyle Hidden
        if ($home_) { $env:OMBRA_HOME = $home_ }
        Write-Host "restarted the engine"
    }
}
