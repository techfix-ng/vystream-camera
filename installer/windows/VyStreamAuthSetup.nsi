Unicode True
RequestExecutionLevel admin
Name "VYSTREAM OBS Authentication Repair"
InstallDir "$PROGRAMFILES64\obs-studio\obs-plugins\64bit"
OutFile "${OUTPUT_DIR}\VYSTREAM-Auth-TLS-Fix-Setup.exe"
ShowInstDetails show
BrandingText "VYSTREAM / TechFixNG"

Page instfiles

Section "Install VYSTREAM authentication repair" SEC_MAIN
  DetailPrint "Close OBS Studio before continuing."
  DetailPrint "Installing the VYSTREAM OBS plugin..."
  SetOutPath "$PROGRAMFILES64\obs-studio\obs-plugins\64bit"
  File "${PLUGIN_DIR}\bin\64bit\obs-srt-camera.dll"
  File /nonfatal "${PLUGIN_DIR}\bin\64bit\libcrypto-3-x64.dll"
  File /nonfatal "${PLUGIN_DIR}\bin\64bit\libssl-3-x64.dll"
  File /nonfatal "${PLUGIN_DIR}\bin\64bit\libcrypto-3.dll"
  File /nonfatal "${PLUGIN_DIR}\bin\64bit\libssl-3.dll"
  File /nonfatal "${PLUGIN_DIR}\bin\64bit\libcrypto-1_1-x64.dll"

  DetailPrint "Installing the Qt TLS backend..."
  SetOutPath "$PROGRAMFILES64\obs-studio\bin\64bit\tls"
  File /nonfatal "${PLUGIN_DIR}\bin\64bit\tls\qschannelbackend.dll"
  File /nonfatal "${PLUGIN_DIR}\bin\64bit\tls\qopensslbackend.dll"

  DetailPrint "Installing a plugin-local TLS backend fallback..."
  SetOutPath "$PROGRAMFILES64\obs-studio\obs-plugins\64bit\tls"
  File /nonfatal "${PLUGIN_DIR}\bin\64bit\tls\qschannelbackend.dll"
  File /nonfatal "${PLUGIN_DIR}\bin\64bit\tls\qopensslbackend.dll"

  DetailPrint "VYSTREAM authentication repair installed."
  MessageBox MB_OK|MB_ICONINFORMATION "VYSTREAM authentication repair installed. Close this installer, then start OBS Studio."
SectionEnd
