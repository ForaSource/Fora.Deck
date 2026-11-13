#ifndef AppVersion
  #error AppVersion must be supplied by build_release.ps1 from the configured VERSION
#endif
#ifndef PackageDir
  #error PackageDir must point to the deployed application
#endif
#ifndef NumericVersion
  #error NumericVersion must contain the numeric Windows version
#endif
#ifndef OutputDir
  #error OutputDir must point to the release directory
#endif

#ifndef Compiler
  #error Compiler must identify the configured toolchain
#endif

[Setup]
AppId={{2B7F5CF1-ABF4-4992-899D-B0A17F8DA51E}
AppName=Fora.Deck
AppVersion={#AppVersion}
AppVerName=Fora.Deck {#AppVersion}
AppPublisher=ForaSource
AppPublisherURL=https://github.com/ForaSource/Fora.Deck
DefaultDirName={localappdata}\Programs\XLibrary
DefaultGroupName=XLibrary
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile={#PackageDir}\LICENSE
SetupIconFile=..\assets\xlibrary.ico
OutputDir={#OutputDir}
OutputBaseFilename=XLibrary-{#AppVersion}-windows-x64-{#Compiler}-setup
VersionInfoVersion={#NumericVersion}
VersionInfoProductVersion={#NumericVersion}
VersionInfoTextVersion={#AppVersion}
VersionInfoProductTextVersion={#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\XLibrary.exe
CloseApplications=yes
RestartApplications=no
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
Type: files; Name: "{app}\portable.flag"
Type: files; Name: "{group}\XLibrary.lnk"
Type: files; Name: "{autodesktop}\XLibrary.lnk"

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Excludes: "portable.flag,data\*"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Fora.Deck"; Filename: "{app}\XLibrary.exe"
Name: "{autodesktop}\Fora.Deck"; Filename: "{app}\XLibrary.exe"; Check: ShouldCreateDesktopShortcut

[Run]
Filename: "{app}\XLibrary.exe"; Description: "{cm:LaunchProgram,Fora.Deck}"; Flags: nowait postinstall skipifsilent

[Code]
var
  HadDesktopShortcut: Boolean;

procedure InitializeWizard;
begin
  { Record the old shortcut before InstallDelete removes its former name. }
  HadDesktopShortcut := FileExists(ExpandConstant('{autodesktop}\XLibrary.lnk'));
end;

function ShouldCreateDesktopShortcut: Boolean;
begin
  Result := HadDesktopShortcut or WizardIsTaskSelected('desktopicon');
end;
