; LibreRamen Windows installer (Inno Setup 6). Built by CI:
;   iscc /DAppVersion=x.y.z installer\windows\libreramen.iss
; expects dist\LibreRamen (GUI), dist\LibreRamenCLI (console) and dist\drivers\wdi-simple.exe.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{6C1F3B7E-5A0D-4C47-9E1B-3F2A8D5C9B10}
AppName=LibreRamen
AppVersion={#AppVersion}
AppPublisher=LibreRamen contributors
AppPublisherURL=https://github.com/ohlincha/libreramen
DefaultDirName={autopf}\LibreRamen
DefaultGroupName=LibreRamen
DisableProgramGroupPage=yes
; administrator rights: needed for the USB drivers
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\..\dist
OutputBaseFilename=LibreRamen-Setup-{#AppVersion}
SetupIconFile=..\..\dist\libreramen.ico
UninstallDisplayIcon={app}\LibreRamen.exe
LicenseFile=..\..\LICENSE
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "drivers"; Description: "Install the USB drivers for the instrument (WinUSB)"; GroupDescription: "Instrument:"

[Files]
Source: "..\..\dist\LibreRamen\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\dist\LibreRamenCLI\*"; DestDir: "{app}\cli"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\dist\drivers\wdi-simple.exe"; DestDir: "{app}\drivers"; Flags: ignoreversion
Source: "install-drivers.ps1"; DestDir: "{app}\drivers"; Flags: ignoreversion
Source: "..\..\dist\drivers\LIBWDI-LICENSE.txt"; DestDir: "{app}\drivers"; Flags: ignoreversion

[Icons]
Name: "{group}\LibreRamen"; Filename: "{app}\LibreRamen.exe"
Name: "{group}\Repair USB drivers"; Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\drivers\install-drivers.ps1"""; IconFilename: "{app}\LibreRamen.exe"
Name: "{autodesktop}\LibreRamen"; Filename: "{app}\LibreRamen.exe"; Tasks: desktopicon

[Run]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\drivers\install-drivers.ps1"""; StatusMsg: "Installing the USB drivers for the instrument..."; Flags: runhidden waituntilterminated; Tasks: drivers
Filename: "{app}\LibreRamen.exe"; Description: "{cm:LaunchProgram,LibreRamen}"; Flags: nowait postinstall skipifsilent
