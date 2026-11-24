# Closes the engine that owns the installed pipe the way the installer does (WM_CLOSE to its
# window), so the next start doesn't record an unclean exit. The pipe only names the process: its
# internal protocol is reserved for ombra-ui. Waits up to 15 s for the process to end.
#   powershell -ExecutionPolicy Bypass -File scripts\stop-engine.ps1
$ErrorActionPreference = 'Stop'
if (-not (Get-Process ombra -ErrorAction SilentlyContinue)) { "engine not running"; exit 0 }
Add-Type -Namespace OmbraStop -Name Win -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool GetNamedPipeServerProcessId(IntPtr pipe, out uint pid);
[DllImport("user32.dll", CharSet = CharSet.Unicode)]
public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
[DllImport("user32.dll")]
public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
[DllImport("user32.dll")]
public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
'@
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'OmbraEngine', [System.IO.Pipes.PipeDirection]::InOut)
$pipe.Connect(3000)
[uint32]$enginePid = 0
$ok = [OmbraStop.Win]::GetNamedPipeServerProcessId($pipe.SafePipeHandle.DangerousGetHandle(), [ref]$enginePid)
$pipe.Dispose()
if (-not $ok) { "could not tell which process owns the pipe"; exit 1 }
# The engine's window is a hidden top-level window (engine.cpp), so a null parent finds it.
$hwnd = [IntPtr]::Zero
$closed = $false
while (($hwnd = [OmbraStop.Win]::FindWindowExW([IntPtr]::Zero, $hwnd, 'OmbraEngineWindow', [NullString]::Value)) -ne [IntPtr]::Zero) {
    [uint32]$owner = 0
    [void][OmbraStop.Win]::GetWindowThreadProcessId($hwnd, [ref]$owner)
    if ($owner -eq $enginePid -and [OmbraStop.Win]::PostMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)) { $closed = $true }
}
if (-not $closed) { "could not post WM_CLOSE to an engine window of pid $enginePid"; exit 1 }
try { Wait-Process -Id $enginePid -Timeout 15 -ErrorAction Stop } catch {}
if (Get-Process -Id $enginePid -ErrorAction SilentlyContinue) { "engine still running after 15 s"; exit 1 }
"engine stopped"
