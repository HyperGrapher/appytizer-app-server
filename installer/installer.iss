#define AppName "Appytizer App Server"
#define AppVersion "1.0.0"

[Setup]
AppId={{D91ED4FC-8CE8-40CA-AB70-9C250EF27EF5}
AppName={#AppName}
AppVersion={#AppVersion}
DefaultDirName={autopf}\Appytizer
DefaultGroupName=Appytizer
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=AppytizerSetup
Compression=lzma2
SolidCompression=yes

[Files]
Source: "..\build\Release\Appytizer.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Release\AppytizerEngine.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Release\AppytizerEngine.exe"; DestName: "AppytizerTlsProvisioner.exe"; Flags: dontcopy
Source: "..\assets\*.png"; DestDir: "{app}\assets"; Flags: ignoreversion
Source: "Output\nginx-1.31.3\*"; DestDir: "{app}\runtime\nginx-1.31.3"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "logs\*,temp\*"
Source: "Output\php85\*"; DestDir: "{app}\runtime\php"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "Output\php85\php.ini"; DestDir: "{commonappdata}\Appytizer\php"; DestName: "php.ini"; Flags: onlyifdoesntexist uninsneveruninstall

[Dirs]
Name: "{commonappdata}\Appytizer\php"; Permissions: users-modify

[InstallDelete]
Type: filesandordirs; Name: "{app}\runtime\nginx"

[Icons]
Name: "{group}\Appytizer"; Filename: "{app}\Appytizer.exe"

[Run]
Filename: "{app}\AppytizerEngine.exe"; Parameters: "--install-service"; Flags: runhidden waituntilterminated
Filename: "{app}\Appytizer.exe"; Description: "Launch Appytizer"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\AppytizerEngine.exe"; Parameters: "--uninstall-service"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveEngine"

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var ResultCode: Integer;
begin
  { Safe when the service does not exist; prevents a locked engine on upgrade. }
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop AppytizerEngine', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  ExtractTemporaryFile('AppytizerTlsProvisioner.exe');
  if not Exec(ExpandConstant('{tmp}\AppytizerTlsProvisioner.exe'), '--provision-tls', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode) or (ResultCode <> 0) then
    Result := 'Appytizer could not provision its trusted local HTTPS certificate authority. Setup was stopped without replacing the existing installation.'
  else
    Result := '';
end;
