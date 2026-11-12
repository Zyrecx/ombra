; Ombra's installer (Inno Setup 6). Built by scripts\package.ps1, which fetches a pinned Inno Setup and
; passes BuildDir and OutputDir; run directly, it reads ..\build\Release and writes ..\out\installer.
;
; Per-user only: %LOCALAPPDATA%\Programs\Ombra, no elevation. The updater (src\ombra\update.h) treats a
; folder with unins000.exe beside ombra.exe as an installer copy, so AppId never changes (a different
; AppId in the same folder would make Inno name the uninstaller unins001.exe).
;
; The updater runs this setup as
;   Ombra-Setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /UPDATE /ENGINEPID=<pid> /LOG="..."
; after which the engine exits on its own. /UPDATE waits for that pid, installs, and starts ombra.exe
; again whether or not the install succeeded (DeinitializeSetup); the engine skips re-applying the
; same version for 24 h, so a setup that keeps failing can't loop. Without /UPDATE (an interactive
; install over a running copy, or winget's silent upgrade) setup closes the engine itself and starts
; it again afterwards.
;
; Everything that is stopped is matched by its exe path under {app}, never by window class alone:
; isolated engines (OMBRA_HOME: tests, ombra-ui --drive) create the same windows from other folders.

#ifndef BuildDir
  #define BuildDir AddBackslash(SourcePath) + "..\build\Release"
#endif
#ifndef OutputDir
  #define OutputDir AddBackslash(SourcePath) + "..\out\installer"
#endif
#define EngineExe BuildDir + "\ombra.exe"
#if !FileExists(EngineExe)
  #error ombra.exe not found: build first (build.ps1)
#endif
; One source for the name, version and publisher: the exe's VERSIONINFO (brand.h, CMake's project
; VERSION and OMBRA_PUBLISHER).
#define AppName GetStringFileInfo(EngineExe, "ProductName")
#define AppVersion GetStringFileInfo(EngineExe, "FileVersion")
#define AppPublisher GetStringFileInfo(EngineExe, "CompanyName")
; Never changes (see the header).
#define AppGuid "A9CB7CB6-5F31-4850-A642-4E51EBD7011D"

[Setup]
AppId={{{#AppGuid}}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL=https://github.com/Zyrecx/ombra
AppSupportURL=https://github.com/Zyrecx/ombra/issues
AppUpdatesURL=https://github.com/Zyrecx/ombra/releases
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 11 (build 22000) and later.
MinVersion=10.0.22000
UninstallDisplayIcon={app}\ombra.exe
UninstallDisplayName={#AppName}
SetupIconFile={#BuildDir}\generated\ombra\ombra.ico
OutputDir={#OutputDir}
OutputBaseFilename=Ombra-Setup
Compression=lzma2/max
SolidCompression=yes
; Light or dark with Windows. The side panel (Welcome and Finished pages) is our own art
; (scripts/wizard-art.py), dark in both themes; no small image on the inner pages. Inno's stock
; images show wherever these are left unset, including the DynamicDark ones.
WizardStyle=modern dynamic
DisableWelcomePage=no
WizardImageFile={#SourcePath}\wizard-164.png,{#SourcePath}\wizard-328.png
WizardImageFileDynamicDark={#SourcePath}\wizard-164.png,{#SourcePath}\wizard-328.png
WizardSmallImageFile=
WizardSmallImageFileDynamicDark=
SetupLogging=yes
; [Code] closes Ombra's own processes first; Restart Manager is the fallback for anything still
; holding a file (an `ombra.exe export` child, say). ombra-ui doesn't handle WM_ENDSESSION, so a
; graceful close would only time out: force it. Nothing is restarted by Restart Manager; the engine
; is started by DeinitializeSetup or the finished page.
CloseApplications=force
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#BuildDir}\ombra.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\ombra-ui.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "{#SourcePath}\..\THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
; ombra-ui opens the overlay, starting the engine first if it isn't running.
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\ombra-ui.exe"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\ombra-ui.exe"; Tasks: desktopicon

[Run]
; A fresh interactive install: start the engine, which opens the first-run setup. When an engine
; was running before, DeinitializeSetup starts it again instead.
Filename: "{app}\ombra.exe"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: postinstall nowait skipifsilent; Check: not EngineWasRunning

[Code]
const
  WM_CLOSE = $0010;
  SYNCHRONIZE = $00100000;
  PROCESS_TERMINATE = $0001;
  PROCESS_QUERY_LIMITED_INFORMATION = $1000;
  WAIT_OBJECT_0 = 0;
  // brand.h OMBRA_INSTALL_ID: the folder under %APPDATA% / %LOCALAPPDATA% and the Run-key value.
  InstallId = 'Ombra';
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  // Window classes: engine.cpp, protocol.h (overlay), toast_window.cpp.
  EngineWindowClass = 'OmbraEngineWindow';
  OverlayWindowClass = 'OmbraOverlayWindowClass';
  ToastWindowClass = 'OmbraToastWindow';

type
  TPidList = array of DWORD;

var
  UpdateMode: Boolean;
  WasRunning: Boolean;
  AppDirSeen: String;

function OpenProcess(Access: DWORD; Inherit: BOOL; Pid: DWORD): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(Handle: THandle; Ms: DWORD): DWORD;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): BOOL;
  external 'CloseHandle@kernel32.dll stdcall';
function TerminateProcess(Handle: THandle; Code: UINT): BOOL;
  external 'TerminateProcess@kernel32.dll stdcall';
function GetTickCount: DWORD;
  external 'GetTickCount@kernel32.dll stdcall';
function QueryFullProcessImageName(Handle: THandle; Flags: DWORD; Name: String; var Size: DWORD): BOOL;
  external 'QueryFullProcessImageNameW@kernel32.dll stdcall';
function FindWindowEx(Parent, After: HWND; ClassName: String; WindowName: Longint): HWND;
  external 'FindWindowExW@user32.dll stdcall';
function GetWindowThreadProcessId(Wnd: HWND; var Pid: DWORD): DWORD;
  external 'GetWindowThreadProcessId@user32.dll stdcall';

function EngineWasRunning: Boolean;
begin
  Result := WasRunning;
end;

function HasSwitch(Name: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), Name) = 0 then Result := True;
end;

function ImagePath(Pid: DWORD): String;
var
  H: THandle;
  Size: DWORD;
begin
  Result := '';
  H := OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, Pid);
  if H = 0 then Exit;
  Size := 1024;
  SetLength(Result, Size);
  if QueryFullProcessImageName(H, 0, Result, Size) then
    SetLength(Result, Size)
  else
    Result := '';
  CloseHandle(H);
end;

function SamePath(A, B: String): Boolean;
begin
  Result := (A <> '') and (CompareText(A, B) = 0);
end;

procedure AddPid(var Pids: TPidList; Pid: DWORD);
var
  I, N: Integer;
begin
  N := GetArrayLength(Pids);
  for I := 0 to N - 1 do
    if Pids[I] = Pid then Exit;
  SetArrayLength(Pids, N + 1);
  Pids[N] := Pid;
end;

// Every top-level window of this class whose process runs ExePath, as pids. With Close, each
// window is also sent WM_CLOSE (the engine's window: its normal exit, as the tray's Exit does).
function FindOwners(ClassName, ExePath: String; Close: Boolean; var Pids: TPidList): Boolean;
var
  Wnd: HWND;
  Pid: DWORD;
begin
  Result := False;
  Wnd := FindWindowEx(0, 0, ClassName, 0);
  while Wnd <> 0 do
  begin
    Pid := 0;
    GetWindowThreadProcessId(Wnd, Pid);
    if (Pid <> 0) and SamePath(ImagePath(Pid), ExePath) then
    begin
      if Close then PostMessage(Wnd, WM_CLOSE, 0, 0);
      AddPid(Pids, Pid);
      Result := True;
    end;
    Wnd := FindWindowEx(0, Wnd, ClassName, 0);
  end;
end;

// True when every pid has exited within TimeoutMs. With Kill, what's left is terminated.
function WaitPids(Pids: TPidList; TimeoutMs: DWORD; Kill: Boolean): Boolean;
var
  I: Integer;
  H: THandle;
  Start, Elapsed, Left: DWORD;
begin
  Result := True;
  Start := GetTickCount;
  for I := 0 to GetArrayLength(Pids) - 1 do
  begin
    H := OpenProcess(SYNCHRONIZE or PROCESS_TERMINATE, False, Pids[I]);
    if H = 0 then Continue;  // already gone
    Elapsed := GetTickCount - Start;
    if Elapsed < TimeoutMs then Left := TimeoutMs - Elapsed else Left := 0;
    if WaitForSingleObject(H, Left) <> WAIT_OBJECT_0 then
    begin
      if Kill then
      begin
        Log(Format('Terminating pid %d', [Pids[I]]));
        TerminateProcess(H, 1);
        WaitForSingleObject(H, 2000);
      end
      else
        Result := False;
    end;
    CloseHandle(H);
  end;
end;

function EngineRunningAt(AppDir: String): Boolean;
var
  Pids: TPidList;
begin
  Result := FindOwners(EngineWindowClass, AddBackslash(AppDir) + 'ombra.exe', False, Pids);
end;

// Closes the overlay and toasts first (an open overlay would start the engine again), then the
// engine. False when the engine is still running after 30 s.
function StopOmbra(AppDir: String): Boolean;
var
  Pids: TPidList;
  Engine, Ui: String;
begin
  Engine := AddBackslash(AppDir) + 'ombra.exe';
  Ui := AddBackslash(AppDir) + 'ombra-ui.exe';
  SetArrayLength(Pids, 0);
  FindOwners(OverlayWindowClass, Ui, True, Pids);
  FindOwners(ToastWindowClass, Ui, True, Pids);
  if GetArrayLength(Pids) > 0 then
  begin
    Log(Format('Closing %d ombra-ui process(es)', [GetArrayLength(Pids)]));
    WaitPids(Pids, 5000, True);
  end;
  SetArrayLength(Pids, 0);
  if FindOwners(EngineWindowClass, Engine, True, Pids) then
  begin
    WasRunning := True;
    Log('Closing the running engine');
  end;
  Result := WaitPids(Pids, 30000, False);
end;

function InitializeSetup: Boolean;
begin
  UpdateMode := HasSwitch('/UPDATE');
  // The engine that handed over to this setup is exiting; it gets started again however setup ends.
  WasRunning := UpdateMode;
  Result := True;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Pid: DWORD;
  Pids: TPidList;
begin
  Result := '';
  AppDirSeen := ExpandConstant('{app}');
  if UpdateMode then
  begin
    Pid := StrToIntDef(ExpandConstant('{param:ENGINEPID|0}'), 0);
    if (Pid <> 0) and SamePath(ImagePath(Pid), AddBackslash(AppDirSeen) + 'ombra.exe') then
    begin
      Log(Format('Waiting for engine pid %d to exit', [Pid]));
      SetArrayLength(Pids, 1);
      Pids[0] := Pid;
      if not WaitPids(Pids, 60000, False) then Log('The engine is still running; closing it');
    end;
  end;
  if not StopOmbra(AppDirSeen) then
    Result := 'Ombra is still running. Exit it from its tray icon, then try again.';
end;

// Where the engine to start again lives: {app}, or the registered install when setup failed with a
// {app} that holds no engine (silent setup runs PrepareToInstall before it rejects a bad /DIR).
function InstalledDir: String;
begin
  Result := AppDirSeen;
  if (Result = '') or not FileExists(AddBackslash(Result) + 'ombra.exe') then
    if not RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{{#AppGuid}}_is1',
      'InstallLocation', Result) then Result := '';
  Result := RemoveBackslashUnlessRoot(Result);
end;

// Runs however setup ends, after a failed or cancelled install too.
procedure DeinitializeSetup;
var
  Dir, Engine: String;
  Code: Integer;
begin
  if not WasRunning then Exit;
  Dir := InstalledDir;
  Log('Engine to start again: ' + Dir);
  if Dir = '' then Exit;
  Engine := AddBackslash(Dir) + 'ombra.exe';
  if FileExists(Engine) and not EngineRunningAt(Dir) then
  begin
    Log('Starting the engine again');
    Exec(Engine, '', Dir, SW_SHOWNORMAL, ewNoWait, Code);
  end;
end;

// ---- Uninstall ----

function InitializeUninstall: Boolean;
begin
  Result := StopOmbra(ExpandConstant('{app}'));
  if not Result then
    SuppressibleMsgBox('Ombra is still running. Exit it from its tray icon, then run the uninstaller again.',
      mbError, MB_OK, IDOK);
end;

procedure DeleteRingFiles(Dir: String);
var
  F: TFindRec;
begin
  if FindFirst(AddBackslash(Dir) + 'ring*.dat', F) then
  try
    repeat
      DeleteFile(AddBackslash(Dir) + F.Name);
    until not FindNext(F);
  finally
    FindClose(F);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  RunValue, Home, ConfigDir, DataDir: String;
begin
  if CurUninstallStep <> usPostUninstall then Exit;

  // The engine writes the autostart value itself. Only this copy's is removed: another copy (a
  // portable one, a build) keeps its own.
  if RegQueryStringValue(HKCU, RunKey, InstallId, RunValue) and
     (Pos(Lowercase(AddBackslash(ExpandConstant('{app}'))), Lowercase(RunValue)) > 0) then
    RegDeleteValue(HKCU, RunKey, InstallId);

  // paths.cpp: OMBRA_HOME moves config and data into one folder.
  Home := GetEnv('OMBRA_HOME');
  if Home <> '' then
  begin
    ConfigDir := Home;
    DataDir := Home;
  end
  else
  begin
    ConfigDir := ExpandConstant('{userappdata}\') + InstallId;
    DataDir := ExpandConstant('{localappdata}\') + InstallId;
  end;

  // Caches that can be large: downloaded updates and a file-backed replay buffer at its default path.
  DelTree(AddBackslash(DataDir) + 'updates', True, True, True);
  DeleteRingFiles(DataDir);

  // Settings, logs and diagnostics stay unless the user asks (a reinstall keeps them). Clips are
  // never touched.
  if not UninstallSilent and
     (MsgBox('Also remove Ombra''s settings, logs and diagnostics? Your clips are kept either way.',
       mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES) then
  begin
    DelTree(ConfigDir, True, True, True);
    DelTree(DataDir, True, True, True);
  end;
end;
