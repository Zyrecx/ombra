# Asks the running engine to exit over its pipe (the tray's Exit path), so the next start doesn't
# record an unclean exit. Waits up to 15 s for the process to end.
#   powershell -ExecutionPolicy Bypass -File scripts\stop-engine.ps1
$ErrorActionPreference = 'Stop'
$procs = Get-Process ombra -ErrorAction SilentlyContinue
if (-not $procs) { "engine not running"; exit 0 }
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'OmbraEngine', [System.IO.Pipes.PipeDirection]::InOut)
$pipe.Connect(3000)
$w = New-Object System.IO.StreamWriter($pipe); $w.AutoFlush = $true
$r = New-Object System.IO.StreamReader($pipe)
$w.WriteLine('{"type":"hello","id":1,"v":2}'); [void]$r.ReadLine()
$w.WriteLine('{"type":"shutdown","id":2}'); [void]$r.ReadLine()
$pipe.Dispose()
try { $procs | Wait-Process -Timeout 15 -ErrorAction Stop } catch {}
if (Get-Process ombra -ErrorAction SilentlyContinue) { "engine still running after 15 s"; exit 1 }
"engine stopped"
