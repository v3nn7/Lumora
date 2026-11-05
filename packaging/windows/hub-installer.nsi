; Lumora Hub installer for Windows (NSIS 3, Unicode).
;
;   makensis /DVERSION=0.2.0-beta.1 /DNUMERIC_VERSION=0.2.0.0 /DSOURCE_DIR=<dist\hub>
;            /DOUT_FILE=<out\lumora-hub-...-setup.exe> packaging\windows\hub-installer.nsi
;
; Installs per user (no administrator rights) into %LOCALAPPDATA%\Programs\Lumora Hub, adds Start
; menu and desktop shortcuts and an entry in "Installed apps". Editors and projects live
; elsewhere (%LOCALAPPDATA%\Lumora) and are never touched by the uninstaller.

Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"

!ifndef VERSION
  !error "pass /DVERSION=... (see the top of this file)"
!endif
!ifndef NUMERIC_VERSION
  !error "pass /DNUMERIC_VERSION=... (see the top of this file)"
!endif
!ifndef SOURCE_DIR
  !error "pass /DSOURCE_DIR=... (see the top of this file)"
!endif
!ifndef OUT_FILE
  !error "pass /DOUT_FILE=... (see the top of this file)"
!endif

!define PRODUCT "Lumora Hub"
!define EXE "bin\lumora-hub.exe"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\LumoraHub"
!define SETTINGS_KEY "Software\Lumora\Hub"

Name "${PRODUCT}"
OutFile "${OUT_FILE}"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\${PRODUCT}"
InstallDirRegKey HKCU "${SETTINGS_KEY}" "InstallDir"
SetCompressor /SOLID lzma
BrandingText "${PRODUCT} ${VERSION}"

VIProductVersion "${NUMERIC_VERSION}"
VIAddVersionKey "ProductName" "${PRODUCT}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${PRODUCT} installer"
VIAddVersionKey "LegalCopyright" "GPL-3.0-or-later"

!define MUI_ICON "${__FILEDIR__}\..\lumora-hub.ico"
!define MUI_UNICON "${__FILEDIR__}\..\lumora-hub.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXE}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SOURCE_DIR}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Polish"

Section "Lumora Hub"
  SetOutPath "$INSTDIR"
  ; Upgrading: drop the previous version's files first so no stale DLL survives. Only when this
  ; folder holds a previous installation (its uninstaller), never a folder the user picked by chance.
  IfFileExists "$INSTDIR\Uninstall.exe" 0 +2
    RMDir /r "$INSTDIR\bin"
  File /r "${SOURCE_DIR}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateShortcut "$SMPROGRAMS\${PRODUCT}.lnk" "$INSTDIR\${EXE}"
  CreateShortcut "$DESKTOP\${PRODUCT}.lnk" "$INSTDIR\${EXE}"

  WriteRegStr HKCU "${SETTINGS_KEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${PRODUCT}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "Lumora contributors"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "URLInfoAbout" "https://lumoraengine.xyz"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\${EXE}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "EstimatedSize" "$0"
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\${PRODUCT}.lnk"
  Delete "$DESKTOP\${PRODUCT}.lnk"
  RMDir /r "$INSTDIR\bin"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\THIRD_PARTY.md"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  DeleteRegKey HKCU "${SETTINGS_KEY}"
SectionEnd
