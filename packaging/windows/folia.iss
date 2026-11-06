; Inno Setup script for the Folia Windows installer.
; Built by .github/workflows/release.yml:
;   iscc /DAppVersion=0.1.0 /DSourceDir=dist\Folia /DOutputDir=out packaging\windows\folia.iss
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist\Folia"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif

[Setup]
AppId={{6B0F3C1E-4D8A-4F7B-9C2E-F0B1A5D3E7C9}
AppName=Folia
AppVersion={#AppVersion}
AppPublisher=Folia
AppPublisherURL=https://github.com/kenichist/folia
AppSupportURL=https://github.com/kenichist/folia/issues
DefaultDirName={autopf}\Folia
DefaultGroupName=Folia
DisableProgramGroupPage=yes
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupIconFile=..\brand\Folia.ico
UninstallDisplayIcon={app}\Folia.exe
OutputDir={#OutputDir}
OutputBaseFilename=Folia-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Folia"; Filename: "{app}\Folia.exe"
Name: "{autodesktop}\Folia"; Filename: "{app}\Folia.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Folia.exe"; Description: "{cm:LaunchProgram,Folia}"; Flags: nowait postinstall skipifsilent
