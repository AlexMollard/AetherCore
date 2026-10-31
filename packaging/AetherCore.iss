; Inno Setup script for the AetherCore editor.
;
; Input is the staged redistributable produced by `cmake --install --component Runtime`,
; passed in as /DPayloadDir. Nothing here reaches into a build tree: if a file is not in
; that staging directory it does not ship, which keeps "what is installed" answerable from
; the payload list in src/app/CMakeLists.txt alone.
;
; Per-user by default, so installation needs no elevation. Runtime/editor execution
; must keep the installed payload immutable: generated scripts belong to the project
; intermediates, while settings and UI state belong to the user-data directory.

#ifndef PayloadDir
  #error PayloadDir must be passed with /DPayloadDir=<path to the staged payload>
#endif
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

#define AppName "AetherCore"
#define AppPublisher "AetherCore"
#define LauncherExe "Launcher.exe"

[Setup]
AppId={{8C0E1D9A-6B4F-4E2A-9C3D-1F7B5A2E8D40}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
; The UTF-8 process manifest requires Windows 10 version 1903 or later.
MinVersion=10.0.18362
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=AetherCoreSetup-{#AppVersion}
SetupIconFile={#SourcePath}\..\resources\branding\aethercore-app-icon.ico
UninstallDisplayIcon={app}\{#LauncherExe}
; The payload is mostly already-compressed data (engine.pak, managed assemblies), so
; solid LZMA earns its slower compression here rather than on incompressible bytes.
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#LauncherExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#LauncherExe}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Run]
Filename: "{app}\{#LauncherExe}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; MSBuild writes obj/bin for the bundled SDK when a project's scripts are first built,
; and the .NET runtime drops native image caches. Neither is tracked by the installer, so
; without this an uninstall leaves the directory behind.
Type: filesandordirs; Name: "{app}\data\sdk\managed\artifacts"
Type: dirifempty; Name: "{app}"
