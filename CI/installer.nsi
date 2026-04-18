!include "MUI2.nsh"

Name "Cloud Closed Captions Plugin ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\obs-studio"
InstallDirRegKey HKLM "SOFTWARE\OBS Studio" ""
RequestExecutionLevel admin

!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "Install"
    SetOutPath "$INSTDIR\obs-plugins\64bit"
    File "${PLUGIN_DLL}"

    ; Create uninstaller
    WriteUninstaller "$INSTDIR\obs-plugins\64bit\uninstall_cloud_captions.exe"

    ; Add to Add/Remove Programs
    WriteRegStr HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\CloudClosedCaptions" \
        "DisplayName" "Cloud Closed Captions Plugin for OBS Studio"
    WriteRegStr HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\CloudClosedCaptions" \
        "UninstallString" "$\"$INSTDIR\obs-plugins\64bit\uninstall_cloud_captions.exe$\""
    WriteRegStr HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\CloudClosedCaptions" \
        "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\CloudClosedCaptions" \
        "Publisher" "Cloud Closed Captions"
    WriteRegDWORD HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\CloudClosedCaptions" \
        "NoModify" 1
    WriteRegDWORD HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\CloudClosedCaptions" \
        "NoRepair" 1
SectionEnd

Section "Uninstall"
    Delete "$INSTDIR\obs-plugins\64bit\obs_google_caption_plugin.dll"
    Delete "$INSTDIR\obs-plugins\64bit\uninstall_cloud_captions.exe"

    DeleteRegKey HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\CloudClosedCaptions"
SectionEnd
