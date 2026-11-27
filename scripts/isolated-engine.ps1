# Starts or stops an isolated engine (OMBRA_HOME) beside the real one, for end-to-end checks: its
# own pipe, config, logs and ring, no tray icon, hotkeys or toasts, and audio off. Video still
# records the desktop. Clips go to <Dir>\clips.
#   powershell -ExecutionPolicy Bypass -File scripts/isolated-engine.ps1 start [-Dir DIR] [-Audio]
#   powershell -ExecutionPolicy Bypass -File scripts/isolated-engine.ps1 stop  [-Dir DIR]
# Talk to it with the same OMBRA_HOME:  $env:OMBRA_HOME = DIR; build\Release\ombra.com status
# start writes a fresh config.json each time; -Audio keeps the default desktop loopback on.
param(
    [Parameter(Mandatory = $true, Position = 0)][ValidateSet('start', 'stop')][string]$Action,
    [string]$Dir = (Join-Path $env:TEMP 'ombra-isolated'),
    [switch]$Audio
)
# Not 'Stop': Windows PowerShell turns a redirected native stderr line into a terminating error.
$root = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $root 'build\Release\ombra.com'
$engine = Join-Path $root 'build\Release\ombra.exe'
if (-not (Test-Path $cli)) { "not built: $cli"; exit 1 }
$Dir = [IO.Path]::GetFullPath($Dir)
$env:OMBRA_HOME = $Dir

if ($Action -eq 'stop') {
    & $cli stop
    exit $LASTEXITCODE
}

& $cli stop
if ($LASTEXITCODE -ne 0) { "an isolated engine in $Dir is still running"; exit 1 }
try { [void](New-Item -ItemType Directory -Force (Join-Path $Dir 'clips') -ErrorAction Stop) } catch { "can't create $Dir\clips: $_"; exit 1 }
$config = [ordered]@{
    'output.root'      = Join-Path $Dir 'clips'
    'buffer.file_path' = Join-Path $Dir 'ring.dat'
}
# "audio.sources": [] falls back to the default desktop loopback, so audio off is a disabled source.
if (-not $Audio) { $config['audio.sources'] = @(@{ id = 'source0'; kind = 'output_loopback'; enabled = $false }) }
# No BOM, whatever the PowerShell version.
[IO.File]::WriteAllText((Join-Path $Dir 'config.json'), (ConvertTo-Json $config -Depth 4), (New-Object Text.UTF8Encoding $false))

$proc = Start-Process $engine -WindowStyle Hidden -PassThru
# status waits for a starting engine only once its instance mutex exists, so poll until then.
$deadline = (Get-Date).AddSeconds(30)
do {
    & $cli status > $null 2> $null
    if ($LASTEXITCODE -eq 0) { break }
    if ($proc.HasExited) { "isolated engine exited (code $($proc.ExitCode)); logs in $Dir\logs"; exit 1 }
    Start-Sleep -Milliseconds 200
} while ((Get-Date) -lt $deadline)
if ($LASTEXITCODE -ne 0) { & $cli stop; "isolated engine did not answer; logs in $Dir\logs"; exit 1 }

# Fail closed: this process's own log line says how many audio sources it will capture.
$line = $null
for ($i = 0; $i -lt 50 -and -not $line; $i++) {
    $log = Get-ChildItem (Join-Path $Dir 'logs') -Filter '*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -Last 1
    if ($log) { $line = Select-String -Path $log.FullName -Pattern "\[E $($proc.Id) .*audio: \d+ sources, (\d+) enabled" | Select-Object -Last 1 }
    if (-not $line) { Start-Sleep -Milliseconds 100 }
}
if (-not $line) { & $cli stop; "no audio line in the isolated engine's log; stopped it"; exit 1 }
$enabled = [int]$line.Matches[0].Groups[1].Value
if (-not $Audio -and $enabled -ne 0) { & $cli stop; "isolated engine has $enabled audio source(s) on; stopped it"; exit 1 }
"isolated engine running in $Dir ($enabled audio source(s) on); stop it with: scripts/isolated-engine.ps1 stop"
