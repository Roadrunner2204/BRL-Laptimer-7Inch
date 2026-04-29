; Inno Setup Script — BRL Studio
;
; Local build (after `pyinstaller tools/brl_studio.spec`):
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DAppVersion=0.1.0 installer\brl_studio_setup.iss
;
; The PyInstaller --onefile artifact is dist/BRL-Studio.exe; this script
; wraps it in a Windows setup wizard with desktop+start-menu icons and a
; clean Apps & Features uninstall entry.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif

#define AppName      "BRL Studio"
#define AppPublisher "Bavarian RaceLabs / roadrunner2204"
#define AppURL       "https://github.com/Roadrunner2204/BRL-Laptimer-7Inch"
#define AppExeName   "BRL-Studio.exe"

[Setup]
; Stable AppId so future installers upgrade in place. NEVER change this.
AppId={{D2E5C8B4-7F39-4A02-A0D1-5B6E8C9A4F71}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} v{#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
AppUpdatesURL={#AppURL}/releases
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
AllowNoIcons=yes
OutputDir=..\dist
OutputBaseFilename=BRL-Studio-Setup-v{#AppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
MinVersion=10.0
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#AppName} v{#AppVersion}
UninstallDisplayIcon={app}\{#AppExeName}

[Languages]
Name: "german";  MessagesFile: "compiler:Languages\German.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\dist\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}";                       Filename: "{app}\{#AppExeName}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";                 Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
