# Installs Ombra for the current user:
#   irm https://github.com/Zyrecx/ombra/releases/latest/download/install.ps1 | iex
# A thin wrapper around Ombra-Setup.exe, which owns the folder, Start menu entry, PATH, updates and
# uninstall: this downloads the setup of the release it came with, checks its SHA-256, runs it
# silently and starts Ombra. Rerunning it reinstalls or updates in place.
#
# scripts\package.ps1 fills in the version and hash and writes out\installer\install.ps1; this
# template refuses to run as it is. Keep the file ASCII without a BOM: GitHub serves release assets
# without a charset, and Windows PowerShell then reads any other byte as the wrong character.
#
# Testing: OMBRA_INSTALL_URL replaces the download URL (file:/// works; the hash is still checked),
# OMBRA_INSTALL_DIR passes /DIR to a first install. With OMBRA_HOME set (an isolated instance) the
# check for a copy running from another folder is skipped, since that instance is separate.

function Install-Ombra {
    $version = '@VERSION@'
    $sha256  = '@SHA256@'
    $url     = "https://github.com/Zyrecx/ombra/releases/download/v$version/Ombra-Setup.exe"
    $appKey  = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{A9CB7CB6-5F31-4850-A642-4E51EBD7011D}_is1'

    if ($sha256 -notmatch '^[0-9a-f]{64}$') { throw 'This is the unfilled install.ps1 template; use the one from a release.' }
    if ($env:OMBRA_INSTALL_URL) { $url = $env:OMBRA_INSTALL_URL }

    if ([Environment]::OSVersion.Version.Build -lt 22000) { throw 'Ombra needs Windows 11.' }
    if (-not [Environment]::Is64BitOperatingSystem) { throw 'Ombra needs 64-bit Windows.' }
    $principal = New-Object Security.Principal.WindowsPrincipal ([Security.Principal.WindowsIdentity]::GetCurrent())
    if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this from a normal (not administrator) PowerShell window: Ombra installs for your user only.'
    }

    # Where setup will install: the folder of an earlier install (setup keeps it), else its default.
    $prev = Get-ItemProperty -LiteralPath $appKey -Name InstallLocation -ErrorAction SilentlyContinue
    if ($prev -and $prev.InstallLocation) { $dir = $prev.InstallLocation }
    elseif ($env:OMBRA_INSTALL_DIR) { $dir = $env:OMBRA_INSTALL_DIR }
    else { $dir = Join-Path $env:LOCALAPPDATA 'Programs\Ombra' }
    $dir = $dir.TrimEnd('\')

    # Setup closes only the Ombra it installs over. Another copy (Scoop, portable) holding the
    # engine's instance lock would make the new one exit at once (engine.cpp StopAsSecondInstance).
    if (-not $env:OMBRA_HOME) {
        $lock = $null
        if ([Threading.Mutex]::TryOpenExisting('Local\OmbraEnginePipeInstance', [ref]$lock)) {
            $lock.Dispose()
            $ours = @(Get-Process ombra -ErrorAction SilentlyContinue |
                Where-Object { $_.Path -and ((Split-Path $_.Path) -eq $dir) })
            if ($ours.Count -eq 0) {
                throw 'Another copy of Ombra is running. Exit it from its tray icon, then run this again.'
            }
        }
    }

    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('ombra-install-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    $protocol = [Net.ServicePointManager]::SecurityProtocol
    try {
        # Windows PowerShell 5.1 may not offer TLS 1.2 by default; GitHub requires it.
        [Net.ServicePointManager]::SecurityProtocol = $protocol -bor [Net.SecurityProtocolType]::Tls12
        $setup = Join-Path $tmp 'Ombra-Setup.exe'
        Write-Host "Downloading Ombra $version..."
        $client = New-Object Net.WebClient
        try { $client.DownloadFile($url, $setup) } finally { $client.Dispose() }

        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $setup).Hash.ToLowerInvariant()
        if ($actual -ne $sha256) { throw "The download is damaged or not the expected file (SHA-256 $actual). Nothing was installed." }

        Write-Host 'Installing...'
        $log  = Join-Path $tmp 'setup.log'
        $setupArgs = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$log`"")
        if (-not $prev -and $env:OMBRA_INSTALL_DIR) { $setupArgs += "/DIR=`"$dir`"" }
        # Wait for setup only: Start-Process -Wait would also wait for the Ombra it restarts.
        $p = Start-Process -FilePath $setup -ArgumentList $setupArgs -PassThru
        $null = $p.Handle
        $p.WaitForExit()
        if ($p.ExitCode -ne 0) {
            $tail = if (Test-Path -LiteralPath $log) { (Get-Content -LiteralPath $log -Tail 5) -join "`n" } else { '' }
            if ($p.ExitCode -eq 7) { throw "Setup stopped: Ombra is still running. Exit it from its tray icon, then run this again.`n$tail" }
            throw "Setup failed (exit code $($p.ExitCode)).`n$tail"
        }
    } finally {
        [Net.ServicePointManager]::SecurityProtocol = $protocol
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }

    $installed = (Get-ItemProperty -LiteralPath $appKey -Name InstallLocation).InstallLocation.TrimEnd('\')
    # Setup puts the folder on the user's PATH for new terminals; this one gets it too.
    if (-not (($env:Path -split ';') | Where-Object { $_.TrimEnd('\') -eq $installed })) {
        $env:Path = $installed + ';' + $env:Path
    }

    # Setup restarts an Ombra it closed; a new install starts here and opens Ombra's setup.
    $running = @(Get-Process ombra -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and ((Split-Path $_.Path) -eq $installed) })
    if ($running.Count -eq 0) {
        Start-Process -FilePath (Join-Path $installed 'ombra.exe') -WorkingDirectory $installed
    }

    Write-Host "Ombra $version is installed in $installed and running in the tray."
    Write-Host 'Open it from the tray icon or the Start menu; uninstall it from Settings > Apps.'
}

Install-Ombra
