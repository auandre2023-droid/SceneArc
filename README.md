# SceneArc

SceneArc 是 Windows 上整理與播放本機影片的播放器：把影片資料夾加入媒體庫，它會依片名與集數整理電影和影集、記住觀看進度，並以 [mpv](https://mpv.io)（libmpv）播放。介面是以 WebView2 載入的網頁 UI，疊在 libmpv 的影片視窗上。

使用說明請見 [docs/guide/index.html](docs/guide/index.html)（基礎與進階兩部分）。

## 功能

- 電影／影集媒體庫、海報牆、繼續播放與最近新增、章節與縮圖預覽
- mpv 播放：D3D11 硬體解碼、HDR 輸出、ASS 字幕（libass）、外掛字幕自動配對
- 音訊：WASAPI，預設經 Windows 混音器輸出；可開啟完美位元（獨佔）輸出
- 畫質：GLSL 著色器放大、AI 超解析放大（VapourSynth + ONNX Runtime DirectML，NVIDIA 可選 TensorRT）
- 補幀：mpv 平滑、MVTools、RIFE、DRBA、AMD FRC
- 線上資料：TMDB、AniList、Bangumi、Kitsu 的片名、簡介、海報、角色與聲優；本機 .nfo 優先
- 區網分享：此版本尚未開放（不開啟任何網路連接埠）

## 原始碼結構

| 路徑 | 內容 |
| --- | --- |
| `host/` | 主程式（C++20，Win32 + DirectComposition + WebView2 + libmpv） |
| `ui/` | 網頁介面（HTML／CSS／JavaScript，無建置步驟）與第三方前端元件 `ui/vendor/` |
| `mpv/` | 效果目錄（`catalog.json`、`models.json`）與 VapourSynth 範本；著色器等 mpv_PlayKit 素材需另外取得（見下方） |
| `installer/` | Inno Setup 安裝檔腳本與繁體中文介面文字 |
| `docs/guide/` | 使用說明（HTML） |
| `tools/` | 開發工具：資產同步、bangumi-data 索引產生、使用者資料備份／還原 |
| `licenses/`、`LICENSE`、`THIRD_PARTY_NOTICES.md` | 本程式與第三方元件的授權 |

## 建置需求

- Windows 10／11 x64
- Visual Studio 2022（或 Build Tools）含「使用 C++ 的桌面開發」、CMake 3.24 以上
- [Inno Setup 6](https://jrsoftware.org/isinfo.php)（只有封裝安裝檔時需要）：`winget install JRSoftware.InnoSetup`

以下第三方檔案不放在 git 中，請自行下載後放到指定位置（都已列在 `.gitignore`）：

| 元件 | 版本 | 放置位置 |
| --- | --- | --- |
| libmpv（shinchiro mpv-winbuild-cmake 的 `mpv-dev-x86_64`） | 20260811-git-f4d13e1c2c | `deps/mpv-sdk/`（內含 `libmpv-2.dll` 與 `include/mpv/`） |
| FFmpeg LGPL shared（BtbN FFmpeg-Builds） | `ffmpeg-n8.1.3-17-gc389840894-win64-lgpl-shared-8.1` | `deps/ffmpeg-sdk/<同名資料夾>/`（含 `bin/`、`include/`、`lib/`） |
| Microsoft.Web.WebView2（NuGet 套件解壓縮） | 1.0.2849.39 | `host/third_party/webview2/` |
| mpv 著色器（GLSL）與 VapourSynth 範例腳本（[hooke007/mpv_PlayKit](https://github.com/hooke007/mpv_PlayKit)） | 20260510 | `mpv/shaders/`、`mpv/vs/` 的 4 個 PlayKit 腳本 |
| AI 執行環境（VapourSynth R73、Python 3.14、k7sfunc 1.8.1、vsort DirectML、ONNX 模型） | 取自 mpv-lazy 20260510 | `vs-runtime/` |

著色器與範例腳本用 `tools/sync-mpv-assets.ps1 -PlayKitDir <解壓縮後的 mpv_PlayKit 資料夾>` 複製（整個 `portable_config/shaders/` 與 `portable_config/vs/` 中的 `MEMC_DRBA_DML`、`MEMC_MVT_LQ`、`MEMC_RIFE_DML`、`MIX_UAI_DML`）。沒有著色器時播放器仍可播放，只是 GLSL 放大與自訂著色器無法使用。

`vs-runtime/` 可以再加上 `-LazyDir <mpv-lazy> -IncludeVsRuntime`，從 mpv-lazy 複製 AI 超分（DirectML）需要的最小集合；補幀用的 `mvtools.dll`、`akarin.dll` 與 `vs-plugins/models/rife*`、`drba` 模型請從同一個 mpv-lazy 套件的 `vs-plugins/` 複製。`vs-runtime/portable.vs` 必須存在，VapourSynth 才會以可攜模式載入同資料夾的 Python。沒有 `vs-runtime/` 時播放器仍可建置與播放，只是 AI 超分與 VapourSynth 補幀無法使用。

## 建置與執行

```bat
build-mpv.cmd
run-mpv.cmd
```

`build-mpv.cmd` 以 CMake + Ninja 建置 `host/build-mpv/VideoPlayer.exe`，並把 `mpv/` 與 `vs-runtime/` 複製到輸出資料夾。開發時播放器從 repo 根目錄的 `ui/` 載入介面，設定與資料存在 repo 根目錄的 `config.json` 與 `host/build-mpv/data/`（都不會進入 git）。

## 封裝安裝檔

```bat
package-mpv.cmd
```

先建置，再把程式、`ui/`、`mpv/`（含上述 PlayKit 素材）、`vs-runtime/` 與授權檔封裝成 `dist/SceneArc-Setup-<版本>.exe`。版本號取自 `VERSION.txt`。安裝檔為每位使用者安裝到 `%LOCALAPPDATA%\Programs\SceneArc`，不需要系統管理員權限；系統缺少 VC++ 執行階段時會執行 Visual Studio 附帶的 `vc_redist.x64.exe`。

## 授權

SceneArc 以 [GNU General Public License v3.0](LICENSE) 發布。mpv、FFmpeg、VapourSynth、著色器與 AI 模型等第三方元件依各自的授權使用，詳見 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 與 `licenses/`。部分 AI 模型採 CC BY-NC 系列授權，僅限非商業用途。

本產品使用 TMDB API，但未經 TMDB 認可或認證。
