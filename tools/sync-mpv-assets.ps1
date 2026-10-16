param(
    [string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$PlayKitDir,
    [string]$LazyDir,
    [switch]$IncludeVsRuntime
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($RepoRoot)) { $RepoRoot = Split-Path -Parent $PSScriptRoot }
$catalogPath = Join-Path $RepoRoot 'mpv\catalog.json'
$shaderSourceRoot = Join-Path $PlayKitDir 'portable_config\shaders'
$shaderTargetRoot = Join-Path $RepoRoot 'mpv\shaders'

if (-not (Test-Path -LiteralPath $catalogPath)) { throw "Catalog not found: $catalogPath" }
if (-not (Test-Path -LiteralPath $shaderSourceRoot)) { throw "PlayKit shader folder not found: $shaderSourceRoot" }

$catalog = Get-Content -LiteralPath $catalogPath -Raw -Encoding UTF8 | ConvertFrom-Json
$shaderFiles = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $catalog.entries) {
    if (-not $entry.files) { continue }
    foreach ($api in $entry.files.PSObject.Properties) {
        foreach ($relative in @($api.Value)) {
            if ([string]::IsNullOrWhiteSpace([string]$relative)) { continue }
            $normalized = ([string]$relative).Replace('/', '\')
            if ([IO.Path]::IsPathRooted($normalized) -or $normalized.Split('\') -contains '..') {
                throw "Catalog entry '$($entry.id)' contains an unsafe shader path: $relative"
            }
            [void]$shaderFiles.Add($normalized)
        }
    }
}
if ($shaderFiles.Count -eq 0) { throw 'The catalog does not reference any GLSL assets.' }

foreach ($relative in ($shaderFiles | Sort-Object)) {
    if (-not (Test-Path -LiteralPath (Join-Path $shaderSourceRoot $relative) -PathType Leaf)) { throw "Referenced shader is missing from PlayKit: $relative" }
}

# The whole PlayKit shader library: the catalog presets use some of it, and the 自訂 shader chain lists every .glsl
# under mpv\shaders. These files are a dependency (not in git, see .gitignore).
New-Item -ItemType Directory -Force -Path $shaderTargetRoot | Out-Null
$copiedShaders = 0
Get-ChildItem -LiteralPath $shaderSourceRoot -Recurse -File -Filter '*.glsl' | ForEach-Object {
    $relative = $_.FullName.Substring($shaderSourceRoot.TrimEnd('\').Length + 1)
    $destination = Join-Path $shaderTargetRoot $relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
    $copiedShaders++
}

# PlayKit's own VapourSynth examples listed in 補幀 / 超解析放大 (open mode). The other mpv\vs scripts are SceneArc's.
$vsTarget = Join-Path $RepoRoot 'mpv\vs'
New-Item -ItemType Directory -Force -Path $vsTarget | Out-Null
foreach ($name in @('MEMC_DRBA_DML.vpy', 'MEMC_MVT_LQ.vpy', 'MEMC_RIFE_DML.vpy', 'MIX_UAI_DML.vpy')) {
    $source = Join-Path $PlayKitDir (Join-Path 'portable_config\vs' $name)
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "PlayKit VapourSynth script is missing: $name" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $vsTarget $name) -Force
}
Set-Content -LiteralPath (Join-Path $RepoRoot 'mpv\ASSETS_VERSION.txt') -Value 'mpv_PlayKit 20260510' -Encoding ASCII

if ($IncludeVsRuntime) {
    if ([string]::IsNullOrWhiteSpace($LazyDir) -or -not (Test-Path -LiteralPath $LazyDir -PathType Container)) {
        throw '-IncludeVsRuntime requires -LazyDir pointing to the local mpv-lazy runtime.'
    }
    $runtimeTarget = Join-Path $RepoRoot 'vs-runtime'
    New-Item -ItemType Directory -Force -Path $runtimeTarget | Out-Null
    $siteSource = Join-Path $LazyDir 'Lib\site-packages'
    $pluginSource = Join-Path $LazyDir 'vs-plugins'
    $modelsSource = Join-Path $pluginSource 'models'

    $requiredRootFiles = @('portable.vs', 'python314.dll', 'python3.dll', 'python314.zip', 'python314._pth', 'VSScript.dll')
    foreach ($name in $requiredRootFiles) {
        $source = Join-Path $LazyDir $name
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Required mpv-lazy runtime file is missing: $name" }
        Copy-Item -LiteralPath $source -Destination (Join-Path $runtimeTarget $name) -Force
    }

    # Keep the embedded Python standard-library extensions and their CRT/TLS/SQLite dependencies.
    $runtimeDlls = @(
        'concrt140.dll', 'libcrypto-3.dll', 'libffi-8.dll', 'libssl-3.dll', 'msvcp140.dll',
        'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll',
        'sqlite3.dll', 'vccorlib140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'vcruntime140_threads.dll'
    )
    foreach ($name in $runtimeDlls) {
        $source = Join-Path $LazyDir $name
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $runtimeTarget $name) -Force
        }
    }
    Get-ChildItem -LiteralPath $LazyDir -File -Filter '*.pyd' | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $runtimeTarget $_.Name) -Force
    }

    $siteTarget = Join-Path $runtimeTarget 'Lib\site-packages'
    New-Item -ItemType Directory -Force -Path $siteTarget | Out-Null
    foreach ($name in @('vapoursynth.dll', 'vapoursynth.pyd', 'vsscript', 'k7sfunc', 'onnx', 'numpy', 'numpy.libs', 'google', 'ml_dtypes')) {
        $source = Join-Path $siteSource $name
        if (-not (Test-Path -LiteralPath $source)) { throw "Required UAI_DML Python package is missing: $name" }
        Copy-Item -LiteralPath $source -Destination (Join-Path $siteTarget $name) -Recurse -Force
    }
    $typingExtensions = Join-Path $siteSource 'typing_extensions.py'
    if (-not (Test-Path -LiteralPath $typingExtensions -PathType Leaf)) { throw 'Required Python module typing_extensions.py is missing.' }
    Copy-Item -LiteralPath $typingExtensions -Destination (Join-Path $siteTarget 'typing_extensions.py') -Force

    foreach ($name in @('vsort.dll', 'vsort\onnxruntime.dll', 'vsort\DirectML.dll')) {
        $source = Join-Path $pluginSource $name
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Required UAI_DML plugin file is missing: $name" }
        $destination = Join-Path $runtimeTarget (Join-Path 'vs-plugins' $name)
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }

    $modelNames = @(
        'the_database_AnimeJaNaiV2L1_x2_fp16_op17.onnx',
        'the_database_AnimeJaNaiV3L1_sharp_HD_x2_fp16_op17.onnx',
        'Sirosky_Ani4Kv2_UltraCompact_x2_fp16_op17.onnx',
        'Phhofm_HFA2kCompact_x2_fp16_op17.onnx',
        'Zarxrax_Anime1080Fixer_SUC_x1_fp16_op17.onnx'
    )
    $modelsTarget = Join-Path $runtimeTarget 'vs-plugins\models'
    New-Item -ItemType Directory -Force -Path $modelsTarget | Out-Null
    foreach ($name in $modelNames) {
        $source = Join-Path $modelsSource $name
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $modelsTarget $name) -Force
        }
    }
    Set-Content -LiteralPath (Join-Path $runtimeTarget 'RUNTIME_VERSION.txt') -Value @(
        'Source: mpv-lazy 20260510',
        'VapourSynth: R73',
        'Python: 3.14.4',
        'k7sfunc: 1.8.1',
        'ORT DirectML: vsort bundle from the local mpv-lazy runtime'
    ) -Encoding ASCII
}

Write-Output "Copied $copiedShaders shaders and 4 PlayKit VapourSynth scripts."
if ($IncludeVsRuntime) { Write-Output 'Copied the portable VapourSynth/DirectML runtime subset.' }
