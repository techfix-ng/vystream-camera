Unicode True
RequestExecutionLevel admin
Name "VyStream OBS Dock 2.9.0"
InstallDir "$APPDATA\obs-studio\plugins\obs-srt-camera"
OutFile "${OUTPUT_DIR}\VyStream-OBS-Dock-v2.9.0-Setup.exe"
ShowInstDetails show
BrandingText "VyStream Professional Broadcast Camera"
VIProductVersion "2.9.0.0"
VIAddVersionKey "ProductName" "VyStream OBS Dock"
VIAddVersionKey "CompanyName" "VyStream / TechFixNG"
VIAddVersionKey "FileDescription" "VyStream OBS Dock installer and legacy cleanup"
VIAddVersionKey "FileVersion" "2.9.0"

Page instfiles

Section "Install VyStream OBS Dock" SEC_MAIN
  SetShellVarContext current
  DetailPrint "Closing OBS Studio..."
  nsExec::ExecToLog 'taskkill /F /IM obs64.exe'
  Sleep 1000

  DetailPrint "Removing previous VyStream and OBS-SRT Camera plugins..."
  RMDir /r "$APPDATA\obs-studio\plugins\obs-srt-camera"
  RMDir /r "$APPDATA\obs-studio\plugins\vystrm-camera"
  RMDir /r "$APPDATA\obs-studio\plugins\vystream-camera"
  Delete "$PROGRAMFILES64\obs-studio\obs-plugins\64bit\obs-srt-camera.dll"
  Delete "$PROGRAMFILES64\obs-studio\obs-plugins\64bit\vystrm-camera.dll"
  RMDir /r "$PROGRAMFILES64\obs-studio\data\obs-plugins\obs-srt-camera"
  RMDir /r "$PROGRAMFILES64\obs-studio\data\obs-plugins\vystrm-camera"

  DetailPrint "Installing the VyStream native OBS dock..."
  SetOutPath "$PROGRAMFILES64\obs-studio\obs-plugins\64bit"
  File "${PLUGIN_DIR}\bin\64bit\obs-srt-camera.dll"
  SetOutPath "$PROGRAMFILES64\obs-studio\data\obs-plugins\obs-srt-camera\locale"
  File "${PLUGIN_DIR}\data\locale\en-US.ini"

  DetailPrint "Configuring Windows Firewall for VyStream discovery and talkback..."
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="VyStream OBS Dock UDP In"'
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="VyStream OBS Dock UDP Out"'
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="VyStream OBS Dock UDP In" dir=in action=allow protocol=UDP localport=45990,46010,46011 profile=any'
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="VyStream OBS Dock UDP Out" dir=out action=allow protocol=UDP remoteport=45990,46010,46011 profile=any'

  CreateDirectory "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall-VyStream-OBS-Dock.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VyStream OBS Dock" "DisplayName" "VyStream OBS Dock"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VyStream OBS Dock" "DisplayVersion" "2.9.0"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VyStream OBS Dock" "Publisher" "VyStream / TechFixNG"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VyStream OBS Dock" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VyStream OBS Dock" "UninstallString" '"$INSTDIR\Uninstall-VyStream-OBS-Dock.exe"'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VyStream OBS Dock" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VyStream OBS Dock" "NoRepair" 1

  DetailPrint "VyStream OBS Dock installation completed."
  MessageBox MB_OK|MB_ICONINFORMATION "VyStream OBS Dock 2.9.0 was installed successfully.$\r$\n$\r$\nClose this installer, then launch OBS Studio normally from its Start Menu or desktop shortcut. The VyStream Camera dock will be available under the Docks menu."
SectionEnd

Section "Uninstall"
  SetShellVarContext current
  nsExec::ExecToLog 'taskkill /F /IM obs64.exe'
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VyStream OBS Dock"
  Delete "$PROGRAMFILES64\obs-studio\obs-plugins\64bit\obs-srt-camera.dll"
  RMDir /r "$PROGRAMFILES64\obs-studio\data\obs-plugins\obs-srt-camera"
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="VyStream OBS Dock UDP In"'
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="VyStream OBS Dock UDP Out"'
SectionEnd
