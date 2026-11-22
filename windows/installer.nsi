; Evict's Windows installer: one file for x64, ARM64 and x86 PCs, installed for the current user (no administrator
; rights), with a Start menu entry and an uninstaller in Settings → Apps. /S installs silently, and /relaunch (the in-app
; updater) opens Evict once done. Built by windows/build.sh with VERSION defined.
Unicode true
ManifestDPIAware true
!include MUI2.nsh
!include x64.nsh
!include FileFunc.nsh

!define UNINSTALL "Software\Microsoft\Windows\CurrentVersion\Uninstall\Evict"

Name "Evict"
OutFile "../.build/Evict-Setup.exe"
InstallDir "$LOCALAPPDATA\Programs\Evict"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "Evict ${VERSION}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "Evict"
VIAddVersionKey "FileDescription" "Evict installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "CompanyName" "vassilidev"
VIAddVersionKey "LegalCopyright" "Copyright (c) 2026 vassilidev, MIT License"

!define MUI_ICON "evict.ico"
!define MUI_UNICON "evict.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "installer.bmp"
!define MUI_FINISHPAGE_RUN "$INSTDIR\Evict.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Open Evict"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; Asks a running Evict to close, then waits until its files can be replaced (10 s at most)
!macro CloseEvict
  FindWindow $0 "Evict"
  IntCmp $0 0 +2
  SendMessage $0 ${WM_CLOSE} 0 0
  StrCpy $1 0
  ${Do}
    ClearErrors
    Delete "$INSTDIR\Evict.exe"
    ${IfNot} ${Errors}
      ${Break}
    ${EndIf}
    IntOp $1 $1 + 1
    ${If} $1 >= 40
      ${Break}
    ${EndIf}
    Sleep 250
  ${Loop}
!macroend

Section
  SetOutPath "$INSTDIR"
  !insertmacro CloseEvict
  ${If} ${IsNativeARM64}
    File "../.build/windows/arm64/*"
  ${ElseIf} ${RunningX64}
    File "../.build/windows/x64/*"
  ${Else}
    File "../.build/windows/x86/*"
  ${EndIf}
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortcut "$SMPROGRAMS\Evict.lnk" "$INSTDIR\Evict.exe"
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegStr HKCU "${UNINSTALL}" "DisplayName" "Evict"
  WriteRegStr HKCU "${UNINSTALL}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALL}" "Publisher" "vassilidev"
  WriteRegStr HKCU "${UNINSTALL}" "DisplayIcon" "$INSTDIR\Evict.exe"
  WriteRegStr HKCU "${UNINSTALL}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINSTALL}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegStr HKCU "${UNINSTALL}" "URLInfoAbout" "https://vassilidev.github.io/evict/"
  WriteRegDWORD HKCU "${UNINSTALL}" "EstimatedSize" $0
  WriteRegDWORD HKCU "${UNINSTALL}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL}" "NoRepair" 1
SectionEnd

Function .onInstSuccess
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/relaunch" $1
  ${IfNot} ${Errors}
    Exec '"$INSTDIR\Evict.exe"'
  ${EndIf}
FunctionEnd

; Only Evict's own files: a custom install folder may hold others
Section "Uninstall"
  !insertmacro CloseEvict
  Delete "$INSTDIR\evict-scan.exe"
  Delete "$INSTDIR\index.html"
  Delete "$INSTDIR\WebView2Loader.dll"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\Evict.lnk"
  DeleteRegKey HKCU "${UNINSTALL}"
  RMDir /r "$LOCALAPPDATA\Evict"  ; scan reports and the web view's data
SectionEnd
