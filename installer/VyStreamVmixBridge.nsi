Unicode true
RequestExecutionLevel admin
Name "VyStream Bridge for vMix 1.3.5"
OutFile "VyStream-vMix-Bridge-v1.3.5-Setup.exe"
InstallDir "$PROGRAMFILES64\VyStream\vMix Bridge"
InstallDirRegKey HKLM "Software\VyStream\vMix Bridge" "InstallDir"
BrandingText "VyStream Professional Broadcast Camera"
Icon "..\assets\vystream.ico"
UninstallIcon "..\assets\vystream.ico"
ShowInstDetails show
SetCompressor /SOLID lzma

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "VyStream Bridge" SEC_MAIN
  SetShellVarContext all
  SetOutPath "$INSTDIR"
  nsExec::ExecToLog 'taskkill /F /IM VyStream-vMix-Bridge.exe'
  Delete "$INSTDIR\VyStream-vMix-Bridge.exe"
  File "..\build\Release\VyStream-vMix-Bridge.exe"
  SetOutPath "$INSTDIR\assets"
  File "..\assets\vystream_camera_icon.png"
  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\VyStream\vMix Bridge" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VyStreamVmixBridge" "DisplayName" "VyStream Bridge for vMix"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VyStreamVmixBridge" "DisplayVersion" "1.3.5"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VyStreamVmixBridge" "Publisher" "TechFixNG"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VyStreamVmixBridge" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  CreateDirectory "$SMPROGRAMS\VyStream"
  CreateShortcut "$SMPROGRAMS\VyStream\VyStream Bridge for vMix.lnk" "$INSTDIR\VyStream-vMix-Bridge.exe"
  CreateShortcut "$DESKTOP\VyStream Bridge for vMix.lnk" "$INSTDIR\VyStream-vMix-Bridge.exe"
  CreateShortcut "$DESKTOP\vMix with VyStream.lnk" "$INSTDIR\VyStream-vMix-Bridge.exe" "--launch-vmix"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "VyStreamVmixBridge"
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="VyStream vMix Discovery"'
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="VyStream vMix Talkback"'
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="VyStream vMix Discovery" dir=in action=allow protocol=UDP localport=45990 program="$INSTDIR\VyStream-vMix-Bridge.exe" profile=private'
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="VyStream vMix Talkback" dir=in action=allow protocol=UDP localport=46011 program="$INSTDIR\VyStream-vMix-Bridge.exe" profile=private'
  Exec '"$INSTDIR\VyStream-vMix-Bridge.exe"'
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  nsExec::ExecToLog 'taskkill /F /IM VyStream-vMix-Bridge.exe'
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="VyStream vMix Discovery"'
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="VyStream vMix Talkback"'
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "VyStreamVmixBridge"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VyStreamVmixBridge"
  DeleteRegKey HKLM "Software\VyStream\vMix Bridge"
  Delete "$DESKTOP\VyStream Bridge for vMix.lnk"
  Delete "$DESKTOP\vMix with VyStream.lnk"
  Delete "$SMPROGRAMS\VyStream\VyStream Bridge for vMix.lnk"
  RMDir "$SMPROGRAMS\VyStream"
  RMDir /r "$INSTDIR"
SectionEnd
