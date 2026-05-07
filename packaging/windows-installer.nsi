Unicode true

!ifndef APP_VERSION
  !define APP_VERSION "0.1.0"
!endif

!ifndef APP_SOURCE_DIR
  !define APP_SOURCE_DIR "..\windows-package"
!endif

Name "WiFiMan Prototype"
OutFile "wifiman_prototype-${APP_VERSION}-windows-x86_64-installer.exe"
InstallDir "$PROGRAMFILES64\WiFiMan Prototype"
RequestExecutionLevel admin

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "Install"
  SetOutPath "$INSTDIR"
  File /r "${APP_SOURCE_DIR}\*"

  CreateDirectory "$SMPROGRAMS\WiFiMan Prototype"
  CreateShortcut "$SMPROGRAMS\WiFiMan Prototype\WiFiMan Prototype.lnk" "$INSTDIR\bin\wifiman_prototype.exe"
  CreateShortcut "$DESKTOP\WiFiMan Prototype.lnk" "$INSTDIR\bin\wifiman_prototype.exe"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\WiFiMan Prototype.lnk"
  Delete "$SMPROGRAMS\WiFiMan Prototype\WiFiMan Prototype.lnk"
  RMDir "$SMPROGRAMS\WiFiMan Prototype"
  RMDir /r "$INSTDIR"
SectionEnd
