; Traditional Chinese (Taiwan) wizard text for the SceneArc installer.
; Loaded after compiler:Default.isl, so any message not listed here falls back to English.

[LangOptions]
LanguageName=繁體中文
LanguageID=$0404
LanguageCodePage=0
DialogFontName=Microsoft JhengHei UI
DialogFontSize=9
WelcomeFontName=Microsoft JhengHei UI
WelcomeFontSize=14

[Messages]

; *** Application titles
SetupAppTitle=安裝程式
SetupWindowTitle=安裝 - %1
UninstallAppTitle=解除安裝
UninstallAppFullTitle=解除安裝 %1

; *** Misc. common
InformationTitle=資訊
ConfirmTitle=確認
ErrorTitle=錯誤

; *** SetupLdr messages
SetupLdrStartupMessage=即將安裝 %1。要繼續嗎？
LdrCannotCreateTemp=無法建立暫存檔，安裝已中止
LdrCannotExecTemp=無法執行暫存資料夾中的檔案，安裝已中止

; *** Startup error messages
LastErrorMessage=%1。%n%n錯誤 %2：%3
SetupFileMissing=安裝資料夾中缺少檔案 %1。請重新取得安裝程式。
SetupFileCorrupt=安裝檔已損毀。請重新取得安裝程式。
SetupFileCorruptOrWrongVer=安裝檔已損毀，或與此版本的安裝程式不相容。請重新取得安裝程式。
InvalidParameter=命令列包含無效的參數：%n%n%1
SetupAlreadyRunning=安裝程式已在執行中。
WindowsVersionNotSupported=此程式不支援您電腦上的 Windows 版本。
WinVersionTooLowError=此程式需要 %1 %2 或更新版本。
WinVersionTooHighError=此程式無法安裝在 %1 %2 或更新版本上。
OnlyOnTheseArchitectures=此程式只能安裝在下列處理器架構的 Windows 上：%n%n%1
SetupAppRunningError=安裝程式偵測到 %1 正在執行。%n%n請先關閉它，然後按「確定」繼續，或按「取消」離開。
UninstallAppRunningError=解除安裝程式偵測到 %1 正在執行。%n%n請先關閉它，然後按「確定」繼續，或按「取消」離開。

; *** Misc. errors
ErrorCreatingDir=無法建立資料夾「%1」
ErrorTooManyFilesInDir=資料夾「%1」內的檔案過多，無法建立檔案

; *** Setup common messages
ExitSetupTitle=結束安裝
ExitSetupMessage=安裝尚未完成。現在結束的話，程式不會被安裝。%n%n您可以稍後再執行安裝程式完成安裝。%n%n要結束安裝嗎？
AboutSetupMenuItem=關於安裝程式(&A)...
AboutSetupTitle=關於安裝程式

; *** Buttons
ButtonBack=< 上一步(&B)
ButtonNext=下一步(&N) >
ButtonInstall=安裝(&I)
ButtonOK=確定
ButtonCancel=取消
ButtonYes=是(&Y)
ButtonYesToAll=全部皆是(&A)
ButtonNo=否(&N)
ButtonNoToAll=全部皆否(&O)
ButtonFinish=完成(&F)
ButtonBrowse=瀏覽(&B)...
ButtonWizardBrowse=瀏覽(&R)...
ButtonNewFolder=新增資料夾(&M)

; *** Common wizard text
ClickNext=按「下一步」繼續，或按「取消」結束安裝。
BrowseDialogTitle=瀏覽資料夾
BrowseDialogLabel=請在下方清單中選擇資料夾，然後按「確定」。
NewFolderName=新資料夾

; *** "Welcome" wizard page
WelcomeLabel1=歡迎使用 [name] 安裝精靈
WelcomeLabel2=即將在您的電腦上安裝 [name/ver]。%n%n建議您先關閉其他應用程式再繼續。

; *** "Select Destination Location" wizard page
WizardSelectDir=選擇安裝位置
SelectDirDesc=要把 [name] 安裝在哪裡？
SelectDirLabel3=安裝程式會將 [name] 安裝到下列資料夾。
SelectDirBrowseLabel=按「下一步」繼續。若要選擇其他資料夾，請按「瀏覽」。
DiskSpaceGBLabel=至少需要 [gb] GB 的可用磁碟空間。
DiskSpaceMBLabel=至少需要 [mb] MB 的可用磁碟空間。
CannotInstallToNetworkDrive=無法安裝到網路磁碟機。
CannotInstallToUNCPath=無法安裝到 UNC 路徑。
InvalidPath=請輸入包含磁碟機代號的完整路徑，例如：%n%nC:\APP%n%n或 UNC 路徑，例如：%n%n\\server\share
InvalidDrive=您選擇的磁碟機或 UNC 共用不存在或無法存取。請選擇其他位置。
DiskSpaceWarningTitle=磁碟空間不足
DiskSpaceWarning=安裝至少需要 %1 KB 的可用空間，但所選磁碟機只剩 %2 KB。%n%n仍要繼續嗎？
DirNameTooLong=資料夾名稱或路徑太長。
InvalidDirName=資料夾名稱無效。
BadDirName32=資料夾名稱不能包含下列字元：%n%n%1
DirExistsTitle=資料夾已存在
DirExists=資料夾：%n%n%1%n%n已經存在。仍要安裝到這個資料夾嗎？
DirDoesntExistTitle=資料夾不存在
DirDoesntExist=資料夾：%n%n%1%n%n不存在。要建立這個資料夾嗎？

; *** "Select Additional Tasks" wizard page
WizardSelectTasks=選擇附加工作
SelectTasksDesc=要執行哪些附加工作？
SelectTasksLabel2=請選擇安裝 [name] 時要執行的附加工作，然後按「下一步」。

; *** "Select Start Menu Folder" wizard page
WizardSelectProgramGroup=選擇「開始」功能表資料夾
SelectStartMenuFolderDesc=要把程式捷徑放在哪裡？
SelectStartMenuFolderLabel3=安裝程式會在下列「開始」功能表資料夾建立捷徑。
SelectStartMenuFolderBrowseLabel=按「下一步」繼續。若要選擇其他資料夾，請按「瀏覽」。
MustEnterGroupName=請輸入資料夾名稱。
GroupNameTooLong=資料夾名稱或路徑太長。
InvalidGroupName=資料夾名稱無效。
BadGroupName=資料夾名稱不能包含下列字元：%n%n%1
NoProgramGroupCheck2=不要建立「開始」功能表資料夾(&D)

; *** "Ready to Install" wizard page
WizardReady=準備安裝
ReadyLabel1=安裝程式已準備好在您的電腦上安裝 [name]。
ReadyLabel2a=按「安裝」開始安裝；若要檢查或變更設定，請按「上一步」。
ReadyLabel2b=按「安裝」開始安裝。
ReadyMemoDir=安裝位置：
ReadyMemoGroup=「開始」功能表資料夾：
ReadyMemoTasks=附加工作：

; *** "Preparing to Install" wizard page
WizardPreparing=準備安裝中
PreparingDesc=安裝程式正在準備將 [name] 安裝到您的電腦。
PreviousInstallNotCompleted=先前的程式安裝或移除尚未完成，需要重新啟動電腦才能完成。%n%n重新啟動後，請再次執行安裝程式以完成 [name] 的安裝。
CannotContinue=安裝無法繼續。請按「取消」離開。
ApplicationsFound=下列應用程式正在使用安裝程式需要更新的檔案。建議讓安裝程式自動關閉這些應用程式。
ApplicationsFound2=下列應用程式正在使用安裝程式需要更新的檔案。建議讓安裝程式自動關閉這些應用程式；安裝完成後，安裝程式會嘗試重新啟動它們。
CloseApplications=自動關閉這些應用程式(&A)
DontCloseApplications=不要關閉這些應用程式(&D)
ErrorCloseApplications=安裝程式無法自動關閉所有應用程式。建議您先關閉所有使用相關檔案的應用程式，再繼續安裝。
PrepareToInstallNeedsRestart=安裝程式必須重新啟動電腦。重新啟動後，請再次執行安裝程式以完成 [name] 的安裝。%n%n要立即重新啟動嗎？

; *** "Installing" wizard page
WizardInstalling=安裝中
InstallingLabel=正在將 [name] 安裝到您的電腦，請稍候。

; *** "Setup Completed" wizard page
FinishedHeadingLabel=[name] 安裝完成
FinishedLabelNoIcons=[name] 已安裝到您的電腦。
FinishedLabel=[name] 已安裝到您的電腦。您可以使用已建立的捷徑啟動程式。
ClickFinish=按「完成」結束安裝。
FinishedRestartLabel=必須重新啟動電腦才能完成 [name] 的安裝。要立即重新啟動嗎？
FinishedRestartMessage=必須重新啟動電腦才能完成 [name] 的安裝。%n%n要立即重新啟動嗎？
YesRadio=是，立即重新啟動電腦(&Y)
NoRadio=否，稍後再重新啟動(&N)
RunEntryExec=執行 %1
RunEntryShellExec=檢視 %1

; *** Installation phase messages
SetupAborted=安裝未完成。%n%n請排除問題後再執行安裝程式。
AbortRetryIgnoreSelectAction=選擇動作
AbortRetryIgnoreRetry=重試(&T)
AbortRetryIgnoreIgnore=忽略錯誤並繼續(&I)
AbortRetryIgnoreCancel=取消安裝
RetryCancelSelectAction=選擇動作
RetryCancelRetry=重試(&T)
RetryCancelCancel=取消

; *** Installation status messages
StatusClosingApplications=正在關閉應用程式...
StatusCreateDirs=正在建立資料夾...
StatusExtractFiles=正在解壓縮檔案...
StatusCreateIcons=正在建立捷徑...
StatusCreateIniEntries=正在寫入 INI 項目...
StatusCreateRegistryEntries=正在寫入登錄項目...
StatusRegisterFiles=正在註冊檔案...
StatusSavingUninstall=正在儲存解除安裝資訊...
StatusRunProgram=正在完成安裝...
StatusRestartingApplications=正在重新啟動應用程式...
StatusRollback=正在復原變更...

; *** Misc. errors
ErrorInternal2=內部錯誤：%1
ErrorFunctionFailedNoCode=%1 失敗
ErrorFunctionFailed=%1 失敗；代碼 %2
ErrorFunctionFailedWithMessage=%1 失敗；代碼 %2。%n%3
ErrorExecutingProgram=無法執行檔案：%n%1

; *** File copying errors
FileAbortRetryIgnoreSkipNotRecommended=略過這個檔案（不建議）(&S)
FileAbortRetryIgnoreIgnoreNotRecommended=忽略錯誤並繼續（不建議）(&I)
SourceIsCorrupted=來源檔案已損毀
SourceDoesntExist=來源檔案「%1」不存在
ExistingFileReadOnly2=現有檔案標示為唯讀，無法取代。
ExistingFileReadOnlyRetry=移除唯讀屬性後重試(&R)
ExistingFileReadOnlyKeepExisting=保留現有檔案(&K)
ErrorReadingExistingDest=讀取現有檔案時發生錯誤：
FileExistsSelectAction=選擇動作
FileExists2=檔案已存在。
FileExistsOverwriteExisting=覆寫現有檔案(&O)
FileExistsKeepExisting=保留現有檔案(&K)
FileExistsOverwriteOrKeepAll=之後的衝突都這樣處理(&D)
ExistingFileNewerSelectAction=選擇動作
ExistingFileNewer2=現有檔案比安裝程式要安裝的檔案更新。
ExistingFileNewerOverwriteExisting=覆寫現有檔案(&O)
ExistingFileNewerKeepExisting=保留現有檔案（建議）(&K)
ExistingFileNewerOverwriteOrKeepAll=之後的衝突都這樣處理(&D)
ErrorChangingAttr=變更現有檔案屬性時發生錯誤：
ErrorCreatingTemp=在安裝資料夾建立檔案時發生錯誤：
ErrorReadingSource=讀取來源檔案時發生錯誤：
ErrorCopying=複製檔案時發生錯誤：
ErrorReplacingExistingFile=取代現有檔案時發生錯誤：
ErrorRestartReplace=重新啟動後取代失敗：
ErrorRenamingTemp=在安裝資料夾重新命名檔案時發生錯誤：

; *** Uninstall display name markings
UninstallDisplayNameMark32Bit=32 位元
UninstallDisplayNameMark64Bit=64 位元
UninstallDisplayNameMarkAllUsers=所有使用者
UninstallDisplayNameMarkCurrentUser=目前使用者

; *** Post-installation errors
ErrorRestartingComputer=安裝程式無法重新啟動電腦，請手動重新啟動。

; *** Uninstaller messages
UninstallNotFound=檔案「%1」不存在，無法解除安裝。
UninstallOpenError=無法開啟檔案「%1」，無法解除安裝
UninstallUnsupportedVer=此版本的解除安裝程式無法辨識解除安裝記錄檔「%1」的格式，無法解除安裝
UninstallUnknownEntry=解除安裝記錄中有無法辨識的項目（%1）
ConfirmUninstall=確定要完整移除 %1 及其所有元件嗎？
UninstallStatusLabel=正在從您的電腦移除 %1，請稍候。
UninstalledAll=已從您的電腦移除 %1。
UninstalledMost=%1 解除安裝完成。%n%n部分項目無法移除，您可以手動刪除。
UninstalledAndNeedsRestart=必須重新啟動電腦才能完成 %1 的解除安裝。%n%n要立即重新啟動嗎？
UninstallDataCorrupted=檔案「%1」已損毀，無法解除安裝

; *** Uninstallation phase messages
WizardUninstalling=解除安裝狀態
StatusUninstalling=正在解除安裝 %1...

; *** Shutdown block reasons
ShutdownBlockReasonInstallingApp=正在安裝 %1。
ShutdownBlockReasonUninstallingApp=正在解除安裝 %1。

[CustomMessages]

NameAndVersion=%1 版本 %2
AdditionalIcons=其他捷徑：
CreateDesktopIcon=建立桌面捷徑(&D)
UninstallProgram=解除安裝 %1
LaunchProgram=啟動 %1
