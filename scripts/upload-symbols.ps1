# Uploads the shipped executables and their PDBs to Sentry, so crash minidumps symbolicate. The
# PE files carry the x64 unwind tables; the PDBs carry names and lines. Sentry matches them to a
# dump by debug ID, so files from any build can be uploaded at any time.
#   $env:SENTRY_AUTH_TOKEN = '...'   # organization token (org:ci)
#   powershell -ExecutionPolicy Bypass -File scripts\upload-symbols.ps1 -Dir build\Release
param([Parameter(Mandatory)][string]$Dir)
$ErrorActionPreference = 'Stop'
if (-not $env:SENTRY_AUTH_TOKEN) { throw 'SENTRY_AUTH_TOKEN is not set' }

$files = foreach ($name in 'ombra.exe', 'ombra.pdb', 'ombra-ui.exe', 'ombra-ui.pdb') {
    $found = @(Get-ChildItem -LiteralPath $Dir -Recurse -File -Filter $name)
    if ($found.Count -ne 1) { throw "Expected one $name under $Dir, found $($found.Count)" }
    $found[0].FullName
}

$cliVersion = '3.8.2'
$cliSha256 = '50009f23c49d6e9ff7624204c7035855303b49c5e326708ee783c1fdf783ac83'
$cli = Join-Path ([IO.Path]::GetTempPath()) "sentry-cli-$cliVersion.exe"
if (-not (Test-Path -LiteralPath $cli) -or (Get-FileHash -LiteralPath $cli).Hash -ne $cliSha256) {
    Invoke-WebRequest "https://github.com/getsentry/sentry-cli/releases/download/$cliVersion/sentry-cli-Windows-x86_64.exe" -OutFile $cli
    if ((Get-FileHash -LiteralPath $cli).Hash -ne $cliSha256) {
        Remove-Item -LiteralPath $cli
        throw "sentry-cli $cliVersion download doesn't match its pinned SHA-256"
    }
}

# --require-all fails if any file isn't usable debug info; --wait fails on server-side processing errors.
& $cli debug-files upload --org zrx --project ombra --require-all --wait @files
if ($LASTEXITCODE -ne 0) { throw "sentry-cli upload failed ($LASTEXITCODE)" }
