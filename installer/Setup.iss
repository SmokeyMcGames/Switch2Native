; SPDX-License-Identifier: GPL-3.0-only
; Compile with tools/Build-Installer.ps1 (Inno Setup 6.7.4).
#ifndef StageDir
  #error StageDir is required
#endif
#define AppName "Switch2NativeControllerDriver"
#include StageDir + "\package.iss"

[Setup]
AppId={{54D60631-6AF5-471C-91B4-CF3C1E4A03A3}
AppName={#AppName}
AppVersion={#PackageVersion}
AppVerName={#AppName} {#PackageVersion} (Development Preview)
AppPublisher=SmokeyMcGames and contributors
AppPublisherURL=https://github.com/SmokeyMcGames/Switch2NativeControllerDriver
AppSupportURL=https://github.com/SmokeyMcGames/Switch2NativeControllerDriver/issues
AppUpdatesURL=https://github.com/SmokeyMcGames/Switch2NativeControllerDriver/releases
DefaultDirName={autopf}\Switch2Native Setup
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableWelcomePage=no
#ifdef PreviewUiOnly
PrivilegesRequired=lowest
#else
PrivilegesRequired=admin
#endif
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.22000
WizardStyle=modern dynamic windows11 hidebevels
WizardSizePercent=120,115
WizardImageFile=artwork.png
WizardImageFileDynamicDark=artwork.png
WizardImageBackColor=#09111f
WizardImageBackColorDynamicDark=#09111f
WizardSmallImageFile=
SetupIconFile=..\assets\controller.ico
UninstallDisplayIcon={app}\controller.ico
UninstallDisplayName={#AppName}
Uninstallable=yes
LicenseFile=..\LICENSE
OutputDir={#OutputDir}
#ifdef PreviewUiOnly
OutputBaseFilename=UI-Preview-DO-NOT-DISTRIBUTE
#else
OutputBaseFilename={#AppName}-{#PackageVersion}-Preview-Setup-x64
#endif
Compression=lzma2
SolidCompression=yes
SetupLogging=yes
CloseApplications=no
RestartApplications=no
AllowCancelDuringInstall=no
SetupMutex=Global\Switch2Native.Setup.Wizard
UninstallLogMode=append
VersionInfoVersion={#PackageVersion}
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} Development Preview Setup

[Files]
Source: "{#StageDir}\out\driver\*"; DestDir: "{tmp}\payload\out\driver"; Flags: dontcopy
Source: "{#StageDir}\tools\*"; DestDir: "{tmp}\payload\tools"; Flags: dontcopy
Source: "{#StageDir}\installer\*"; DestDir: "{tmp}\payload\installer"; Flags: dontcopy
Source: "{#StageDir}\*"; DestDir: "{app}"; Excludes: "package.iss"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{commonprograms}\{#AppName}\Getting started"; Filename: "{app}\Getting started.html"
Name: "{commonprograms}\{#AppName}\Uninstall"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\Getting started.html"; Description: "Read the connection and control guide"; Flags: shellexec postinstall skipifsilent unchecked runasoriginaluser
Filename: "ms-settings:bluetooth"; Description: "Open Windows Bluetooth settings to pair a controller"; Flags: shellexec postinstall skipifsilent unchecked runasoriginaluser

[Messages]
WelcomeLabel1=Your controllers. Ready for Windows.
WelcomeLabel2=Install {#AppName} with gamepad input, native motion sensors, optical and gyro mouse control, and individual controller settings.%n%nSetup includes everything needed on a Windows 11 x64 PC. No developer tools or separate downloads are required.%n%nDevelopment preview - review the signing information on the next pages.
FinishedHeadingLabel=Ready to connect
FinishedLabel=Pair a Joy-Con 2 or Pro Controller in Windows Bluetooth settings, or connect the Pro Controller by USB.%n%nIf a newly paired controller says Not connected, press a normal button once. The tray appears when a controller connects.%n%nRight-click the tray icon for Settings, mouse modes and Joy-Con grouping.
ConfirmUninstall=Remove %1 and its driver, service and recorded development certificates?%n%nController profiles and Windows Bluetooth pairings will be kept.

[Code]
var
  TrustPage: TInputOptionWizardPage;
  RequiresRestart: Boolean;
  DriverReady: Boolean;

procedure InitializeWizard;
begin
  WizardForm.WizardSmallBitmapImage.Visible := False;
  WizardForm.WelcomeLabel1.Font.Size := 22;
  WizardForm.WelcomeLabel1.Height := ScaleY(78);
  WizardForm.WelcomeLabel2.Top := WizardForm.WelcomeLabel1.Top + WizardForm.WelcomeLabel1.Height + ScaleY(12);
  WizardForm.WelcomeLabel2.Height := ScaleY(220);
  TrustPage := CreateInputOptionPage(wpLicense, 'Development signing',
    'A local trust decision for this preview',
    'This package is signed with a temporary project development certificate. It is not Microsoft-certified or signed by a publicly trusted publisher.' + #13#10#13#10 +
    'Installing adds its public certificate to this PC''s Root and TrustedPublisher stores. Uninstall removes the certificates recorded by the driver. Windows security settings are not disabled.' + #13#10#13#10 +
    'Certificate: {#CertificateThumbprint}' + #13#10 + 'Valid until: {#CertificateExpiry}' + #13#10#13#10 +
    'Only continue on a test PC if you trust this project and downloaded this file from its official repository.', False, False);
  TrustPage.Add('I trust this development package and allow setup to install its certificate.');
  TrustPage.Values[0] := ExpandConstant('{param:TRUSTDEVCERT|0}') = '1';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = TrustPage.ID) and not TrustPage.Values[0] then begin
    MsgBox('Select the certificate consent box to continue, or cancel setup.', mbInformation, MB_OK);
    Result := False;
  end;
end;

function RunBackend(Root, Action: String; var Detail: String): Boolean;
var
  Code: Integer;
  ResultPath, Arguments: String;
begin
  ResultPath := ExpandConstant('{tmp}\driver-result.ini');
  DeleteFile(ResultPath);
  Arguments := '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + Root +
    '\installer\Invoke-Setup.ps1" -Action ' + Action + ' -ResultFile "' + ResultPath + '"';
  if Action = 'Install' then Arguments := Arguments + ' -TrustLocalDevelopmentCertificate';
  Result := Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Arguments,
    Root, SW_HIDE, ewWaitUntilTerminated, Code);
  if Result then Result := (Code = 0) or (Code = 3010);
  RequiresRestart := RequiresRestart or (Code = 3010);
  Detail := GetIniString('Setup', 'Message', 'Setup could not finish. See %ProgramData%\Switch2Native\logs and the setup log in your temporary folder.', ResultPath);
  Log(Detail);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Detail: String;
begin
  Result := '';
#ifdef PreviewUiOnly
  Result := 'UI preview only. No drivers, files or trust will be installed.';
  Exit;
#endif
  if DriverReady then Exit;
  if not TrustPage.Values[0] then begin
    Result := 'Explicit development-certificate consent is required.';
    Exit;
  end;
  ExtractTemporaryFiles('{tmp}\payload\*');
  WizardForm.PreparingLabel.Caption := 'Setting up the Windows controller devices and background service. This may take a few minutes.';
  if not RunBackend(ExpandConstant('{tmp}\payload'), 'Install', Detail) then begin
    Result := Detail;
    Exit;
  end;
  DriverReady := True;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfoInfo, MemoTypeInfoInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := '{#AppName} {#PackageVersion}' + NewLine + NewLine +
    'Installs or updates the controller driver, manager service and connection-aware tray.' + NewLine + NewLine +
    'Keeps controller profiles and Windows Bluetooth pairings.' + NewLine +
    'Adds an uninstall entry to Windows Installed apps.' + NewLine +
    'Includes the GPL license, third-party notices and exact corresponding source.' + NewLine + NewLine +
    'Windows may request a restart. Setup will report that result; it will not force a restart.';
end;

function NeedRestart: Boolean;
begin
  Result := RequiresRestart;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and RequiresRestart then begin
    WizardForm.YesRadio.Checked := False;
    WizardForm.NoRadio.Checked := True;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Detail: String;
begin
  if CurUninstallStep = usUninstall then
    if not RunBackend(ExpandConstant('{app}'), 'Uninstall', Detail) then
      RaiseException(Detail + #13#10 + 'Uninstall stopped. Its recovery files have been kept.');
end;

function UninstallNeedRestart: Boolean;
begin
  Result := RequiresRestart;
end;
