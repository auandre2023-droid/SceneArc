# licenses/

這個資料夾收錄本程式附帶的第三方元件所用授權的全文。哪個元件用哪份授權，請看上一層的 THIRD_PARTY_NOTICES.md。
package-mpv.cmd 會把整個資料夾、LICENSE 和 THIRD_PARTY_NOTICES.md 一起打包進安裝目錄。

| 檔案 | 授權 | 適用元件 |
|---|---|---|
| GPL-3.0.txt | GNU GPL v3.0 | 本程式、vs-mlrt / vsort、k7sfunc 內附的 vsmlrt、ACNet / ARNet（Anime4KCPP）、FSRCNNX、rvfc-polyfill |
| GPL-2.0.txt | GNU GPL v2.0 | mpv / libmpv（GPL-2.0-or-later 建置）、MVTools |
| LGPL-2.1.txt | GNU LGPL v2.1 | FFmpeg（LGPL-2.1-or-later）、VapourSynth、uosc 字型 |
| LGPL-3.0.txt | GNU LGPL v3.0（要和 GPL-3.0.txt 一起看） | CuNNy、RAISR / NNEDI3（mpv-prescalers）、akarin |
| mpv-Copyright.txt | mpv 各檔案的授權說明 | mpv / libmpv |
| PSF-Python-3.14.txt | Python Software Foundation License | Python 3.14 embeddable |
| Apache-2.0.txt | Apache License 2.0 | Material Icons 字型、OpenCC 字典資料 |
| MIT-Anime4K.txt | MIT | Anime4K shader |
| MIT-ArtCNN.txt | MIT | ArtCNN、Ani4Kv2 / AniSD ArtCNN shader |
| MIT-Practical-RIFE.txt | MIT | RIFE 模型 |
| MIT-DRBA.txt | MIT | DRBA |
| MIT-onnxruntime.txt | MIT | ONNX Runtime |
| MIT-opencc-js.txt | MIT | opencc-js |
| MIT-FidelityFX-FSR.txt | MIT | AMD FSR1 / CAS shader |
| MIT-NVIDIAImageScaling.txt | MIT | NVIDIA Image Scaling / Sharpen shader |
| CC-BY-NC-SA-4.0.txt | CC BY-NC-SA 4.0 | AnimeJaNai 模型（V2、V3.1、SD、V2L1、V3L1） |
| AnimeJaNai-LICENSE.txt | 上游 LICENSE 原檔（CC BY-NC-SA 4.0） | AnimeJaNai 模型 |
| CC-BY-NC-4.0.txt | CC BY-NC 4.0 | Ani4Kv2 UltraCompact |
| Upscale-Hub-LICENSE.md | 上游 LICENSE 原檔（CC BY-NC 4.0） | Ani4Kv2 UltraCompact |
| CC-BY-4.0.txt | CC BY 4.0 | 2xHFA2kCompact、bangumi-data 索引 |
| mpv_PlayKit-LICENSE.md | mpv_PlayKit 授權說明（各檔案授權寫在檔內） | mpv/shaders、mpv/vs、k7sfunc |

另外有幾份授權聲明放在元件自己的資料夾裡，跟著該資料夾一起發行：

- `ui/vendor/jassub/COPYRIGHT`：JASSUB 及其子專案
- `ui/vendor/rvfc-polyfill/LICENSE`
- `ui/vendor/bangumi-data/ATTRIBUTION.txt`
- 各 shader 檔頭的授權註解

## 原始碼

GPL / LGPL 元件的原始碼可從上游取得：

- mpv：https://github.com/mpv-player/mpv（Windows 建置：https://github.com/shinchiro/mpv-winbuild-cmake）
- FFmpeg：https://ffmpeg.org/（建置：https://github.com/BtbN/FFmpeg-Builds）
- VapourSynth：https://github.com/vapoursynth/vapoursynth
- vs-mlrt：https://github.com/AmusementClub/vs-mlrt
- MVTools：https://github.com/dubhater/vapoursynth-mvtools
- akarin：https://github.com/AkarinVS/vapoursynth-plugin

本程式自身的原始碼見本 repository。

授權全文取自 gnu.org、apache.org、creativecommons.org 及各上游 repository，抓取日期 2026-10-16。
