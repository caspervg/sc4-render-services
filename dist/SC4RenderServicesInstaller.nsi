!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "nsDialogs.nsh"
!include "FileFunc.nsh"
!cd "${__FILEDIR__}"

!define APP_NAME "SC4 Render Services"
!ifndef APP_VERSION
  !define APP_VERSION "dev"
!endif
!ifndef INSTALLER_OUTPUT
  !define INSTALLER_OUTPUT "SC4RenderServices-${APP_VERSION}-Setup.exe"
!endif
!ifndef THIRD_PARTY_NOTICES_PATH
  !define THIRD_PARTY_NOTICES_PATH "THIRD_PARTY_NOTICES.txt"
!endif
!ifndef IMGUI_DLL_PATH
  !define IMGUI_DLL_PATH "..\cmake-build-release-visual-studio\Release\imgui.dll"
!endif
!ifndef RENDER_SERVICES_DLL_PATH
  !define RENDER_SERVICES_DLL_PATH "..\cmake-build-release-visual-studio\Release\SC4RenderServices.dll"
!endif
!ifndef RENDER_SERVICES_INI_PATH
  !define RENDER_SERVICES_INI_PATH "..\SC4RenderServices.ini"
!endif
!ifndef README_PATH
  !define README_PATH "README.txt"
!endif
!ifndef LICENSE_PATH
  !define LICENSE_PATH "..\LICENSE.txt"
!endif

!define SERVICE_REG_KEY "Software\SC4RenderServices"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\SC4RenderServices"

Name "${APP_NAME} ${APP_VERSION}"
OutFile "${INSTALLER_OUTPUT}"
Unicode True
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show

Var Dialog
Var GameRoot
Var SC4PluginsDir
Var HGameRoot
Var HPluginsDir
Var HBrowseGameRoot
Var HBrowsePluginsDir
Var SupportDir
Var GameExePath

!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${THIRD_PARTY_NOTICES_PATH}"
Page custom ConfigurePathsPage ConfigurePathsPageLeave
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"

Function .onInit
  SetShellVarContext current
  Call DetectDefaultGameRoot
  StrCpy $SC4PluginsDir "$DOCUMENTS\SimCity 4\Plugins"
FunctionEnd

Function DetectDefaultGameRoot
  ; SC4 is a 32-bit app, so read from the 32-bit registry view.
  StrCpy $GameRoot "$PROGRAMFILES32\SimCity 4 Deluxe Edition"
  SetRegView 32
  ReadRegStr $0 HKLM "SOFTWARE\Maxis\SimCity 4" "Install Dir"
  ${If} $0 != ""
    StrCpy $GameRoot $0
  ${EndIf}
FunctionEnd

Function ValidateGameExecutable
  StrCpy $GameExePath "$GameRoot\Apps\SimCity 4.exe"

  ${IfNot} ${FileExists} "$GameExePath"
    MessageBox MB_OK|MB_ICONSTOP "Could not find '$GameExePath'.$\r$\n$\r$\nSC4 Render Services requires the SimCity 4 1.1.641.x executable in the selected game folder."
    Abort
  ${EndIf}

  ClearErrors
  GetDLLVersion "$GameExePath" $0 $1
  ${If} ${Errors}
    MessageBox MB_OK|MB_ICONSTOP "Could not read the version information from '$GameExePath'.$\r$\n$\r$\nSC4 Render Services requires SimCity 4 version 1.1.641.x."
    Abort
  ${EndIf}

  IntOp $2 $0 >> 16
  IntOp $2 $2 & 0xFFFF
  IntOp $3 $0 & 0xFFFF
  IntOp $4 $1 >> 16
  IntOp $4 $4 & 0xFFFF
  IntOp $5 $1 & 0xFFFF

  ${If} $2 != 1
  ${OrIf} $3 != 1
  ${OrIf} $4 != 641
    MessageBox MB_OK|MB_ICONSTOP "Unsupported SimCity 4 version detected in '$GameExePath'.$\r$\n$\r$\nFound: $2.$3.$4.$5$\r$\nRequired: 1.1.641.x$\r$\n$\r$\nPlease install the 1.1.641 update before continuing."
    Abort
  ${EndIf}
FunctionEnd

Function WarnIfNo4GBPatch
  ClearErrors
  FileOpen $0 "$GameExePath" r
  ${If} ${Errors}
    Return
  ${EndIf}

  FileSeek $0 60 SET
  ClearErrors
  FileReadByte $0 $1
  FileReadByte $0 $2
  FileReadByte $0 $3
  FileReadByte $0 $4
  ${If} ${Errors}
    FileClose $0
    Return
  ${EndIf}

  IntOp $5 $2 << 8
  IntOp $5 $5 + $1
  IntOp $6 $3 << 16
  IntOp $5 $5 + $6
  IntOp $6 $4 << 24
  IntOp $5 $5 + $6

  IntOp $5 $5 + 22
  FileSeek $0 $5 SET
  ClearErrors
  FileReadByte $0 $1
  FileReadByte $0 $2
  ${If} ${Errors}
    FileClose $0
    Return
  ${EndIf}
  FileClose $0

  IntOp $3 $2 << 8
  IntOp $3 $3 + $1
  IntOp $3 $3 & 0x20

  ${If} $3 == 0
    MessageBox MB_OK|MB_ICONEXCLAMATION "The selected SimCity 4 executable does not appear to have the 4GB patch applied.$\r$\n$\r$\nSC4 Render Services can still be installed, but applying the 4GB patch is recommended for stability."
  ${EndIf}
FunctionEnd

Function ConfigurePathsPage
  nsDialogs::Create 1018
  Pop $Dialog
  ${If} $Dialog == error
    Abort
  ${EndIf}

  ${NSD_CreateLabel} 0u 0u 100% 18u "Choose where to install ${APP_NAME} files."
  ${NSD_CreateLabel} 0u 24u 100% 10u "SimCity 4 game root (contains Apps folder):"
  ${NSD_CreateDirRequest} 0u 36u 82% 12u "$GameRoot"
  Pop $HGameRoot
  ${NSD_CreateButton} 84% 36u 16% 12u "Browse..."
  Pop $HBrowseGameRoot
  ${NSD_OnClick} $HBrowseGameRoot OnBrowseGameRoot

  ${NSD_CreateLabel} 0u 56u 100% 10u "SimCity 4 Plugins directory:"
  ${NSD_CreateDirRequest} 0u 68u 82% 12u "$SC4PluginsDir"
  Pop $HPluginsDir
  ${NSD_CreateButton} 84% 68u 16% 12u "Browse..."
  Pop $HBrowsePluginsDir
  ${NSD_OnClick} $HBrowsePluginsDir OnBrowsePluginsDir

  nsDialogs::Show
FunctionEnd

Function OnBrowseGameRoot
  Pop $0
  nsDialogs::SelectFolderDialog "Select SimCity 4 game root folder" "$GameRoot"
  Pop $0
  ${If} $0 != error
    StrCpy $GameRoot $0
    ${NSD_SetText} $HGameRoot $GameRoot
  ${EndIf}
FunctionEnd

Function OnBrowsePluginsDir
  Pop $0
  nsDialogs::SelectFolderDialog "Select SimCity 4 Plugins folder" "$SC4PluginsDir"
  Pop $0
  ${If} $0 != error
    StrCpy $SC4PluginsDir $0
    ${NSD_SetText} $HPluginsDir $SC4PluginsDir
  ${EndIf}
FunctionEnd

Function ConfigurePathsPageLeave
  ${NSD_GetText} $HGameRoot $GameRoot
  ${NSD_GetText} $HPluginsDir $SC4PluginsDir

  ${If} $GameRoot == ""
    MessageBox MB_OK|MB_ICONEXCLAMATION "Game root cannot be empty."
    Abort
  ${EndIf}

  ${If} $SC4PluginsDir == ""
    MessageBox MB_OK|MB_ICONEXCLAMATION "Plugins directory cannot be empty."
    Abort
  ${EndIf}

  ${IfNot} ${FileExists} "$GameRoot\Apps\*.*"
    MessageBox MB_OK|MB_ICONSTOP "Could not find '$GameRoot\Apps'.$\r$\n$\r$\nPlease select your SimCity 4 game root folder (the folder that contains 'Apps')."
    Abort
  ${EndIf}

  Call ValidateGameExecutable
  Call WarnIfNo4GBPatch
FunctionEnd

Section "Install"
  SetShellVarContext current

  CreateDirectory "$GameRoot\Apps"
  SetOutPath "$GameRoot\Apps"
  File "${IMGUI_DLL_PATH}"

  CreateDirectory "$SC4PluginsDir"
  SetOutPath "$SC4PluginsDir"
  File "${RENDER_SERVICES_DLL_PATH}"
  SetOverwrite off
  File "${RENDER_SERVICES_INI_PATH}"
  SetOverwrite on

  ${GetParent} "$SC4PluginsDir" $0
  StrCpy $SupportDir "$0\SC4RenderServices"
  CreateDirectory "$SupportDir"
  SetOutPath "$SupportDir"
  File "${README_PATH}"
  File "${LICENSE_PATH}"
  File "${THIRD_PARTY_NOTICES_PATH}"

  WriteUninstaller "$SupportDir\Uninstall-SC4RenderServices.exe"

  WriteRegDWORD HKLM "${SERVICE_REG_KEY}" "Version" 1
  WriteRegStr HKLM "${SERVICE_REG_KEY}" "GameRoot" "$GameRoot"
  WriteRegStr HKLM "${SERVICE_REG_KEY}" "PluginsDir" "$SC4PluginsDir"
  WriteRegStr HKLM "${SERVICE_REG_KEY}" "SupportDir" "$SupportDir"

  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "${APP_NAME} ${APP_VERSION}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "SC4 Render Services"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$SupportDir"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" "$\"$SupportDir\Uninstall-SC4RenderServices.exe$\""
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Function un.onInit
  SetShellVarContext current
  ReadRegStr $GameRoot HKLM "${SERVICE_REG_KEY}" "GameRoot"
  ReadRegStr $SC4PluginsDir HKLM "${SERVICE_REG_KEY}" "PluginsDir"
  ReadRegStr $SupportDir HKLM "${SERVICE_REG_KEY}" "SupportDir"

  ${If} $GameRoot == ""
    ; SC4 is a 32-bit app, so read from the 32-bit registry view.
    StrCpy $GameRoot "$PROGRAMFILES32\SimCity 4 Deluxe Edition"
    SetRegView 32
    ReadRegStr $0 HKLM "SOFTWARE\Maxis\SimCity 4" "Install Dir"
    ${If} $0 != ""
      StrCpy $GameRoot $0
    ${EndIf}
  ${EndIf}
  ${If} $SC4PluginsDir == ""
    StrCpy $SC4PluginsDir "$DOCUMENTS\SimCity 4\Plugins"
  ${EndIf}
  ${If} $SupportDir == ""
    ${GetParent} "$SC4PluginsDir" $0
    StrCpy $SupportDir "$0\SC4RenderServices"
  ${EndIf}
FunctionEnd

Section "Uninstall"
  SetShellVarContext current

  Delete "$GameRoot\Apps\imgui.dll"
  Delete "$SC4PluginsDir\SC4RenderServices.dll"
  Delete "$SupportDir\README.txt"
  Delete "$SupportDir\LICENSE.txt"
  Delete "$SupportDir\THIRD_PARTY_NOTICES.txt"
  Delete "$SupportDir\Uninstall-SC4RenderServices.exe"
  RMDir "$SupportDir"

  DeleteRegKey HKLM "${SERVICE_REG_KEY}"
  DeleteRegKey HKLM "${UNINSTALL_KEY}"
SectionEnd
