# Third-Party Notices / 第三方元件與授權

本程式（Video Player）自身的原始碼以 **GNU General Public License v3.0** 授權，全文見 [LICENSE](LICENSE)。

本程式在原始碼或發行包中使用、附帶了以下第三方元件。每個元件依其原作者的授權使用，**這些授權不因本程式的 GPL-3.0 而改變**。各元件的授權全文附在發行包的 `licenses/` 資料夾，或可從下列網址取得。

> ⚠️ **非商業限制**：部分 AI 模型採用 CC BY-NC / CC BY-NC-SA 授權，**只限非商業用途**。TMDB 免費 API 也只限非商業用途。要拿本程式做商業用途的人，必須自行移除或替換這些元件，並自行取得相應授權。

狀態欄位：✅ 已從上游 LICENSE 確認 · ⚠️ 尚未確認（發行前必須確認）

---

## 1. AI 模型（ONNX）

位置：`vs-runtime/vs-plugins/models/`。各檔案的來源網址與 SHA-256 記錄在 `vs-runtime/vs-plugins/models/SOURCES.txt`。

| 模型 | 作者 / 來源 | 授權 | 狀態 | 修改 |
|---|---|---|---|---|
| 2x_AnimeJaNai V2 Compact / UltraCompact / SuperUltraCompact | the-database — https://github.com/the-database/mpv-AnimeJaNai | CC BY-NC-SA 4.0 | ✅ | 有。`*_shape3.onnx` 只把輸出 channel 維度的標註從符號名改成 3，權重不變；未修改的原檔保留在 `upstream-original/` |
| 2x_AnimeJaNai HD V3.1（Balanced / Performance / Sharp1）、SD V1beta34 | the-database — https://github.com/the-database/mpv-AnimeJaNai | CC BY-NC-SA 4.0 | ✅ | 無 |
| AnimeJaNai V2 L1、V3 L1 Sharp HD（`the_database_*_op17.onnx`） | the-database（經 mpv_PlayKit 轉檔） | CC BY-NC-SA 4.0 | ✅ | 由 mpv_PlayKit 轉成 fp16 / opset 17 |
| Ani4Kv2 UltraCompact（`Sirosky_*`） | Sirosky — https://github.com/Sirosky/Upscale-Hub | CC BY-NC 4.0 | ✅ | 由 mpv_PlayKit 轉成 fp16 / opset 17 |
| 2xHFA2kCompact（`Phhofm_*`） | Philip Hofmann (Phhofm) — https://huggingface.co/Phips/2xHFA2kCompact | CC BY 4.0 | ✅ | 由 mpv_PlayKit 轉成 fp16 / opset 17 |
| Anime1080Fixer SUC x1（`Zarxrax_*`） | Zarxrax | 未確認 | ⚠️ | 由 mpv_PlayKit 轉成 fp16 / opset 17 |
| RIFE v4.6 / v4.6 ensemble / v4.25 lite / v4.26 / v4.26 heavy | hzwer — https://github.com/hzwer/Practical-RIFE；ONNX 轉檔：AmusementClub/vs-mlrt（external-models release） | MIT（模型）；vs-mlrt 程式碼為 GPL-3.0 | ✅ | 無 |
| distilDRBA v2 lite | DRBA：routineLife1 — https://github.com/routineLife1/DRBA（MIT）；distil 版本的出處 | MIT（DRBA）/ distil 版本未確認 | ⚠️ | 無 |

CC 授權條文：
- CC BY-NC-SA 4.0：https://creativecommons.org/licenses/by-nc-sa/4.0/
- CC BY-NC 4.0：https://creativecommons.org/licenses/by-nc/4.0/
- CC BY 4.0：https://creativecommons.org/licenses/by/4.0/

## 2. 播放核心與執行環境

| 元件 | 來源 | 授權 | 狀態 |
|---|---|---|---|
| mpv / libmpv（`libmpv-2.dll`，shinchiro Windows build） | https://github.com/mpv-player/mpv | GPL-2.0-or-later（本程式採用的 GPL 建置） | ✅ |
| FFmpeg（LGPL shared build） | https://ffmpeg.org/ ；建置：https://github.com/BtbN/FFmpeg-Builds | LGPL-2.1-or-later | ✅ |
| VapourSynth R73 | https://github.com/vapoursynth/vapoursynth | LGPL-2.1 | ✅ |
| Python 3.14（embeddable） | https://www.python.org/ | PSF License | ✅ |
| vs-mlrt / vsort（ONNX Runtime + DirectML 外掛） | https://github.com/AmusementClub/vs-mlrt | GPL-3.0 | ✅ |
| ONNX Runtime / DirectML | Microsoft | MIT | ✅ |
| k7sfunc 1.8.1 | hooke007 — https://github.com/hooke007/mpv_PlayKit | 依 mpv_PlayKit LICENSE.MD 各檔案標示；內附的 vsmlrt 為 GPL-3.0 | ⚠️ 確認 k7sfunc 本體授權 |
| MVTools（`mvtools.dll`） | https://github.com/dubhater/vapoursynth-mvtools | GPL-2.0 | ✅ |
| akarin（`akarin.dll`） | https://github.com/AkarinVS/vapoursynth-plugin | LGPL-3.0 | ✅ |
| Microsoft Visual C++ Runtime | Microsoft | Visual Studio 可再散發元件授權 | ✅ |
| NVIDIA TensorRT（選用，只有 NVIDIA 套件使用） | NVIDIA | NVIDIA TensorRT SLA | ⚠️ 確認可再散發的檔案範圍 |

FFmpeg、VapourSynth 等 LGPL 元件以動態連結方式使用。對應版本的原始碼可從上列網址取得。

## 3. mpv shader 與設定（來自 mpv_PlayKit 20260510）

位置：`mpv/shaders/`、`mpv/vs/`、`mpv/templates/`。來源：hooke007/mpv_PlayKit — https://github.com/hooke007/mpv_PlayKit 。依其 LICENSE.MD，各檔案使用不同授權，授權寫在檔案內。檔案沒有標示授權的，以上游最新版本為準。

| Shader 系列 | 原作者 / 來源 | 授權 |
|---|---|---|
| Anime4K | bloc97 — https://github.com/bloc97/Anime4K | MIT |
| ArtCNN、Ani4Kv2/AniSD ArtCNN | Artoriuz — https://github.com/Artoriuz/ArtCNN | MIT |
| CuNNy | funnyplanter — https://github.com/funnyplanter/CuNNy | LGPL-3.0 |
| RAISR、NNEDI3（EDI） | bjin — https://github.com/bjin/mpv-prescalers | LGPL-3.0 |
| ACNet / ARNet | TianZerL — https://github.com/TianZerL/Anime4KCPP | GPL-3.0 |
| FSRCNNX | igv — https://github.com/igv/FSRCNN-TensorFlow | GPL-3.0 |
| AMD FSR1 / CAS | AMD FidelityFX | MIT |
| NVIDIA Image Scaling / Sharpen | NVIDIA NIS | MIT |
| AiUpscale | Alexkral — https://github.com/Alexkral/AiUpscale | 見檔頭 ⚠️ |
| 其他（AA、Deband、nlmeans、guided、eq、color、Pixel-art、QCOM 等） | 各檔頭標示 | 見檔頭 |

字型：`MaterialIconsRound-Regular.otf`（Google，Apache-2.0）、`uosc_textures.ttf`（tomasklaen/uosc，LGPL-2.1）。

## 4. 介面（`ui/vendor/`）

| 元件 | 版本 | 授權 | 狀態 |
|---|---|---|---|
| JASSUB（含 libass、brotli 等，見 `ui/vendor/jassub/COPYRIGHT`） | 1.8.8 | MIT（各子專案見 COPYRIGHT） | ✅ |
| opencc-js（字典資料來自 OpenCC，Apache-2.0） | 1.4.2 | MIT | ✅ |
| rvfc-polyfill | 1.0.8 | GPL-3.0 | ✅ |
| bangumi-data 索引（見 `ui/vendor/bangumi-data/ATTRIBUTION.txt`） | 0.3.231 | CC BY 4.0（已轉換為索引） | ✅ |

## 5. 線上資料來源

本程式在使用者啟用後，會向以下服務取得資料。資料的權利屬於各服務及其貢獻者，使用時須遵守各服務的使用條款。程式內「設定 → 關於」也有顯示這些來源。

- **TMDB** — This product uses the TMDB API but is not endorsed or certified by TMDB. https://www.themoviedb.org （TMDB logo：`ui/brand/third-party/tmdb-blue-short.svg`，取自官方 Logos & Attribution 頁面，未經修改）。免費 API 只限非商業用途。
- **AniList** — https://anilist.co （依 AniList API 使用條款）
- **Bangumi 番組計劃** — https://bgm.tv （依 Bangumi API 使用條款）
- **Kitsu** — https://kitsu.app （依 Kitsu API 使用條款）

使用者必須自行申請 TMDB API 金鑰；本程式不附帶任何金鑰。

---

本文件是元件清單與授權摘要，不構成法律意見。如有出入，以各元件上游的授權條文為準。
