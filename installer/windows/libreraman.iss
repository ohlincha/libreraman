; LibreRaman Windows installer (Inno Setup 6), shared by the Python and the C++ builds. Built by CI:
;   Python: iscc /DAppVersion=x.y.z installer\windows\libreraman.iss
;           expects dist\LibreRaman (GUI), dist\LibreRamanCLI (console), dist\libreraman.ico
;   C++:    iscc /DAppVersion=x.y.z /DCpp installer\windows\libreraman.iss
;           expects dist-cpp\LibreRaman (LibreRaman.exe and LibreRamanCLI.exe with their DLLs, one folder),
;           dist-cpp\libreraman.ico
; Both expect dist\drivers\wdi-simple.exe and dist\drivers\LIBWDI-LICENSE.txt (artifact of wdi.yml).
; Both use the same AppId: installing one replaces the other (the leftovers of the other kind are deleted).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifdef Cpp
  #define AppSource "..\..\dist-cpp\LibreRaman"
  #define IconFile "..\..\dist-cpp\libreraman.ico"
  #define OutputName "LibreRaman-" + AppVersion + "-Windows-x64-Setup"
#else
  #define AppSource "..\..\dist\LibreRaman"
  #define CliSource "..\..\dist\LibreRamanCLI"
  #define IconFile "..\..\dist\libreraman.ico"
  #define OutputName "LibreRaman-Setup-" + AppVersion
#endif

[Setup]
AppId={{6C1F3B7E-5A0D-4C47-9E1B-3F2A8D5C9B10}
AppName=LibreRaman
AppVersion={#AppVersion}
AppPublisher=LibreRaman contributors
AppPublisherURL=https://github.com/ohlincha/libreraman
DefaultDirName={autopf}\LibreRaman
DefaultGroupName=LibreRaman
DisableProgramGroupPage=yes
; administrator rights: needed for the USB drivers
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1809 or later (Qt 6)
MinVersion=10.0.17763
OutputDir=..\..\dist
OutputBaseFilename={#OutputName}
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\LibreRaman.exe
LicenseFile=..\..\LICENSE
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "drivers"; Description: "Install the USB drivers for the instrument (WinUSB)"; GroupDescription: "Instrument:"

[InstallDelete]
#ifdef Cpp
; files of the Python build
Type: filesandordirs; Name: "{app}\_internal"
Type: filesandordirs; Name: "{app}\cli"
#else
; files of the C++ build
Type: files; Name: "{app}\Qt6*.dll"
Type: files; Name: "{app}\LibreRamanCLI.exe"
Type: files; Name: "{app}\libusb-1.0.dll"
Type: files; Name: "{app}\hidapi.dll"
Type: files; Name: "{app}\msvcp140*.dll"
Type: files; Name: "{app}\vcruntime140*.dll"
Type: files; Name: "{app}\concrt140.dll"
Type: files; Name: "{app}\vccorlib140.dll"
Type: filesandordirs; Name: "{app}\platforms"
Type: filesandordirs; Name: "{app}\styles"
Type: filesandordirs; Name: "{app}\imageformats"
Type: filesandordirs; Name: "{app}\iconengines"
Type: filesandordirs; Name: "{app}\sqldrivers"
Type: filesandordirs; Name: "{app}\tls"
Type: filesandordirs; Name: "{app}\networkinformation"
Type: filesandordirs; Name: "{app}\generic"
Type: filesandordirs; Name: "{app}\translations"
#endif

[Files]
Source: "{#AppSource}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
#ifdef CliSource
Source: "{#CliSource}\*"; DestDir: "{app}\cli"; Flags: ignoreversion recursesubdirs createallsubdirs
#endif
Source: "..\..\dist\drivers\wdi-simple.exe"; DestDir: "{app}\drivers"; Flags: ignoreversion
Source: "install-drivers.ps1"; DestDir: "{app}\drivers"; Flags: ignoreversion
Source: "..\..\dist\drivers\LIBWDI-LICENSE.txt"; DestDir: "{app}\drivers"; Flags: ignoreversion

[Icons]
Name: "{group}\LibreRaman"; Filename: "{app}\LibreRaman.exe"
Name: "{group}\Repair USB drivers"; Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\drivers\install-drivers.ps1"""; IconFilename: "{app}\LibreRaman.exe"
Name: "{autodesktop}\LibreRaman"; Filename: "{app}\LibreRaman.exe"; Tasks: desktopicon

[Run]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\drivers\install-drivers.ps1"""; StatusMsg: "Installing the USB drivers for the instrument..."; Flags: runhidden waituntilterminated; Tasks: drivers; Check: not WizardSilent
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\drivers\install-drivers.ps1"" -Quiet"; StatusMsg: "Installing the USB drivers for the instrument..."; Flags: runhidden waituntilterminated; Tasks: drivers; Check: WizardSilent
Filename: "{app}\LibreRaman.exe"; Description: "{cm:LaunchProgram,LibreRaman}"; Flags: nowait postinstall skipifsilent
