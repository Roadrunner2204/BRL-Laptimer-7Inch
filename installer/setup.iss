; Inno Setup Script – TRX Excel Konverter
; Wird von GitHub Actions automatisch mit /DAppVersion=x.y.z aufgerufen.
; Lokales Bauen (nach "pyinstaller tools/trx_to_excel_gui.spec"):
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DAppVersion=1.0.0 installer\setup.iss

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

#define AppName      "TRX Excel Konverter"
#define AppPublisher "CAN Checked / roadrunner2204"
#define AppURL       "https://github.com/roadrunner2204/bmw-data-display-v0.1"
#define AppExeName   "TRX-Excel-Konverter.exe"
#define AppDir       "TRX-Excel-Konverter"

[Setup]
; Eindeutige App-ID – niemals ändern (sonst erkennt Windows es als neue App)
AppId={{8F3A1D2E-5C4B-4F9A-B2D7-1E6F8A3C5D9B}
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
; Ausgabe-Dateiname
OutputDir=..\dist
OutputBaseFilename=TRX-Excel-Konverter-Setup-v{#AppVersion}
; Kompression
Compression=lzma2/ultra64
SolidCompression=yes
; Modernes Wizard-Design
WizardStyle=modern
; Erlaubt Installation ohne Admin-Rechte (in %LOCALAPPDATA%)
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
; Mindest-Windows-Version: Windows 10
MinVersion=10.0
; Kein 32-Bit only Flag (läuft nativ als 64-Bit)
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "german";  MessagesFile: "compiler:Languages\German.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Kompletter PyInstaller-Ausgabeordner
Source: "..\dist\{#AppDir}\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\{#AppDir}\*";             DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}";                       Filename: "{app}\{#AppExeName}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";                 Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; \
  Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; \
  Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Aufräumen: von der App angelegte Dateien im Installationsordner entfernen
Type: filesandordirs; Name: "{app}"
