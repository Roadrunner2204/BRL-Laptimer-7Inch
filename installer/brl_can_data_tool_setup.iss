; Inno Setup Script - BRL Can Data Tool
;
; Lokales Bauen (nach "pyinstaller tools/brl_can_data_tool.spec"):
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DAppVersion=1.0.6 installer\brl_can_data_tool_setup.iss
;
; GitHub Actions baut dies automatisch, siehe .github/workflows/build-installers.yml

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

#define AppName      "BRL Can Data Tool"
#define AppPublisher "Bavarian RaceLabs / roadrunner2204"
#define AppURL       "https://github.com/Roadrunner2204/BRL-Laptimer-7Inch"
#define AppExeName   "BRL-Can-Data-Tool.exe"

[Setup]
; Eindeutige AppId — Inno Setup nutzt diese, um existierende Installationen
; automatisch zu erkennen und saubere Upgrades durchzufuehren. NIEMALS aendern,
; sonst koennen User alte Installationen nicht mehr ueber den Installer
; ersetzen und muessten erst haendisch deinstallieren.
AppId={{C1F4B7A3-6D28-4E91-9F1D-8A4C7B2E5D60}
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
OutputBaseFilename=BRL-Can-Data-Tool-Setup-v{#AppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
MinVersion=10.0
ArchitecturesInstallIn64BitMode=x64compatible
; Uninstaller-Eintrag in "Apps & Features" / "Programme entfernen" mit
; Publisher-Link und Support-URL:
UninstallDisplayName={#AppName} v{#AppVersion}
UninstallDisplayIcon={app}\{#AppExeName}

[Languages]
Name: "german";  MessagesFile: "compiler:Languages\German.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Onefile-PyInstaller-Build → genau eine .exe, alle Libs/Assets sind darin
; eingebettet. Wird einfach in den Programm-Ordner kopiert.
Source: "..\dist\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}";                       Filename: "{app}\{#AppExeName}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";                 Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
