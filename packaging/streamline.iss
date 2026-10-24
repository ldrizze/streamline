; Inno Setup 6 script: packages dist/ (cmake --install + tools/collect-licenses.sh)
; into a per-user installer, no admin rights.
;   ISCC /DAppVersion=1.2.3 packaging\streamline.iss  ->  build\installer\Streamline-Setup-1.2.3.exe

#ifndef AppVersion
  #define AppVersion "0.3.1"
#endif
; Numeric x.y.z for the file version resource (AppVersion may be "0.3.1-ci.12").
#ifndef NumericVersion
  #define NumericVersion "0.3.1"
#endif
#define DistDir "..\dist"

[Setup]
AppId={{8C2F7A1E-4B6D-4E1A-9F3C-2D7B5A9E6C41}
AppName=Streamline
AppVersion={#AppVersion}
AppVerName=Streamline {#AppVersion}
VersionInfoVersion={#NumericVersion}
AppPublisher=Luis Fedrizze
AppPublisherURL=https://github.com/ldrizze/streamline
AppSupportURL=https://github.com/ldrizze/streamline/issues
DefaultDirName={autopf}\Streamline
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Per-process audio loopback needs Windows 10 2004 (build 19041).
MinVersion=10.0.19041
LicenseFile={#DistDir}\LICENSE.txt
OutputDir=..\build\installer
OutputBaseFilename=Streamline-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\resources\icon\streamline.ico
UninstallDisplayIcon={app}\bin\streamline.exe
CloseApplications=yes
; Tells Explorer to refresh its icon cache: upgrades keep the same exe path, so
; shortcuts would otherwise keep showing the previous version's icon.
ChangesAssociations=yes

[Languages]
Name: "ptbr"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#DistDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; Left over by 0.1.0, which shipped its own signaling server.
Type: files; Name: "{app}\bin\streamline-signal.exe"
Type: files; Name: "{autoprograms}\Streamline Signal Server.lnk"

[Icons]
Name: "{autoprograms}\Streamline"; Filename: "{app}\bin\streamline.exe"
Name: "{autodesktop}\Streamline"; Filename: "{app}\bin\streamline.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\streamline.exe"; Description: "{cm:LaunchProgram,Streamline}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; GStreamer plugin registry cache written by the app.
Type: filesandordirs; Name: "{localappdata}\streamline"
