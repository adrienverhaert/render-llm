param(
    [ValidateSet("Debug", "RelWithDebInfo", "Release")]
    [string]$Configuration = "Release",
    [string]$BuildPath = "build\native",
    [switch]$ConfigureOnly,
    [switch]$AddressSanitizer,
    [string]$OidnBin,
    [string]$DxcBin,
    [switch]$RequireGpuRuntime,
    [switch]$AllowGpuFallback
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path $PSScriptRoot -Parent
$sourceDir = Join-Path $repoRoot "CppProject"
$buildDir = if ([IO.Path]::IsPathRooted($BuildPath)) {
    [IO.Path]::GetFullPath($BuildPath)
} else {
    [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildPath))
}
$repoRootFull = [IO.Path]::GetFullPath($repoRoot).TrimEnd('\') + '\'
if (-not $buildDir.StartsWith($repoRootFull, [StringComparison]::OrdinalIgnoreCase)) {
    throw "BuildPath must stay inside the repository: $buildDir"
}
$dependencyDir = Join-Path $repoRoot ".v"
$generatedHeader = Join-Path $sourceDir "Generated\Scripts.hpp"
$generatedSources = @(Get-ChildItem -LiteralPath (Join-Path $sourceDir "Generated") -Filter "*.cpp" -File)

if (-not (Test-Path -LiteralPath $generatedHeader -PathType Leaf) -or $generatedSources.Count -eq 0) {
    throw "Committed generated C++ is incomplete. Expected Generated\Scripts.hpp and Generated\*.cpp."
}

$hasOidn = -not [string]::IsNullOrWhiteSpace($OidnBin)
$hasDxc = -not [string]::IsNullOrWhiteSpace($DxcBin)
if ($hasOidn -xor $hasDxc) {
    throw "OidnBin and DxcBin must be supplied together."
}
if (-not $ConfigureOnly -and -not $hasOidn -and -not $AllowGpuFallback) {
    throw "Verified OIDN and DXC runtimes are required for a native package. Supply OidnBin and DxcBin, or explicitly use -AllowGpuFallback for a diagnostic D3D11/atrous build."
}

$vcpkgRoot = $env:VCPKG_ROOT
if (-not $vcpkgRoot) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw "VCPKG_ROOT is not set and Visual Studio vswhere.exe was not found."
    }

    $vsPath = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
    if (-not $vsPath) {
        throw "Visual Studio Build Tools with MSBuild were not found."
    }
    $vcpkgRoot = Join-Path $vsPath "VC\vcpkg"
}

$toolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path -LiteralPath $toolchain -PathType Leaf)) {
    throw "vcpkg toolchain not found at $toolchain"
}

Write-Host "Using $($generatedSources.Count) committed generated C++ translation units."
& cmake -S $sourceDir -B $buildDir -G "Visual Studio 18 2026" -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    "-DVCPKG_INSTALLED_DIR=$dependencyDir" `
    "-DVCPKG_TARGET_TRIPLET=x64-windows-static" `
    "-DRENDER_LLM_ASAN=$($AddressSanitizer.IsPresent)"
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if (-not $ConfigureOnly) {
    # Fail before staging if a running editor/agent owns this runtime. Never
    # partially replace an installation while its child process is using it.
    $runtimeDir = Join-Path $buildDir $Configuration
    $runtimePattern = [regex]::Escape([IO.Path]::GetFullPath($runtimeDir))
    $owners = @(Get-CimInstance Win32_Process | Where-Object {
        $_.Name -in @('render-llm.exe', 'node.exe') -and
        ($_.ExecutablePath -match $runtimePattern -or ($_.CommandLine -replace '/', '\') -match $runtimePattern)
    })
    if ($owners.Count -gt 0) { throw "Close this runtime's editor/Agent before packaging (PID $($owners.ProcessId -join ', '))." }
    & cmake --build $buildDir --config $Configuration -- /m
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $runtimeDir = Join-Path $buildDir $Configuration
    $datafilesDir = Join-Path $repoRoot "GmProject\datafiles"
    $dataSource = Join-Path $datafilesDir "Data"
    if (-not (Test-Path -LiteralPath $dataSource -PathType Container)) {
        throw "Required runtime folder not found: $dataSource"
    }
    $dataDestination = Join-Path $runtimeDir "Data"
    $dataDestinationFull = [IO.Path]::GetFullPath($dataDestination)
    $runtimeRootForData = [IO.Path]::GetFullPath($runtimeDir).TrimEnd('\') + '\'
    if (-not $dataDestinationFull.StartsWith($runtimeRootForData, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace Data outside the native runtime: $dataDestinationFull"
    }
    # Data contains projects, chat history and exported images as well as shipped
    # resources. Overlay resources only; never recursively replace this directory.
    New-Item -ItemType Directory -Path $dataDestination -Force | Out-Null
    # Canon contains only armor/elytra/shield templates for retired content flows.
    $retiredDataFolders = @("Canon", "Minecraft", "ShinyTrims", "Splashes")
    $resourceRoots = @('Fonts', 'Languages', 'Libraries', 'Render', 'languages.midata')
    Get-ChildItem -LiteralPath $dataSource -Force | Where-Object {
        $_.Name -in $resourceRoots
    } | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $dataDestination -Recurse -Force
    }
    # The retained startup still expects the legacy conversion-map shape, but the
    # reduced product no longer ships Minecraft block/item/biome conversion data.
    Copy-Item -LiteralPath (Join-Path $repoRoot "tools\runtime-data\legacy.midata") `
        -Destination (Join-Path $dataDestination "legacy.midata") -Force

    # Keep every retained error/compatibility key while removing retired public
    # branding from the English catalog that the native product actually loads.
    $englishCatalog = Join-Path $dataDestination "Languages\english.milanguage"
    if (-not (Test-Path -LiteralPath $englishCatalog -PathType Leaf)) {
        throw "Required English runtime catalog not found: $englishCatalog"
    }
    $englishText = [IO.File]::ReadAllText($englishCatalog)
    $englishText = $englishText.Replace("Mine-imator", "render-llm").Replace("Minecraft", "legacy content").Replace(".miproject", ".renderllm")
    [IO.File]::WriteAllText($englishCatalog, $englishText, [Text.UTF8Encoding]::new($false))
    if ($englishText.Contains("Mine-imator") -or $englishText.Contains("Minecraft") `
        -or $englishText.Contains(".miproject")) {
        throw "Retired product branding remains in the staged English catalog."
    }
    foreach ($tier in @(
        '"assistantmodelhaiku": "Bajo"',
        '"assistantmodelsonnet": "Medio"',
        '"assistantmodelopus": "Alto"',
        '"assistantmodelhaikushort": "Bajo"',
        '"assistantmodelsonnetshort": "Medio"',
        '"assistantmodelopusshort": "Alto"'
    )) {
        if (-not $englishText.Contains($tier)) {
            throw "The staged Agent level catalog is missing: $tier"
        }
    }

    # Particle presets belong to the retired MI-3 content workflow. They are not
    # part of the render-llm document schema or reduced Shape/Light/Camera surface.
    $retiredParticles = Join-Path $runtimeDir "Particles"
    if (Test-Path -LiteralPath $retiredParticles -PathType Container) {
        Remove-Item -LiteralPath $retiredParticles -Recurse -Force
    }

    # Build and stage the native visual-agent host from its lockfile. The running
    # application must never install or compile dependencies on first launch.
    $assistantSource = Join-Path $repoRoot "ASSISTANT"
    $assistantRuntime = Join-Path $runtimeDir "ASSISTANT"
    $npmCommand = (Get-Command npm -ErrorAction Stop).Source
    Push-Location $assistantSource
    try {
        & $npmCommand ci --ignore-scripts --no-audit --no-fund
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & $npmCommand run build:runtime
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    finally {
        Pop-Location
    }

    $runtimeRootFull = [IO.Path]::GetFullPath($runtimeDir).TrimEnd('\') + '\'
    $assistantRuntimeFull = [IO.Path]::GetFullPath($assistantRuntime)
    if (-not $assistantRuntimeFull.StartsWith($runtimeRootFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace assistant runtime outside the native build: $assistantRuntimeFull"
    }
    if (Test-Path -LiteralPath $assistantRuntimeFull -PathType Container) {
        Remove-Item -LiteralPath $assistantRuntimeFull -Recurse -Force
    }
    New-Item -ItemType Directory -Path (Join-Path $assistantRuntimeFull "prompts") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $assistantRuntimeFull "tools") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $assistantRuntimeFull "dist") -Force | Out-Null
    Copy-Item -Path (Join-Path $assistantSource "dist-runtime\*") -Destination (Join-Path $assistantRuntimeFull "dist") -Recurse
    # The scripted MI-3 fake remains available to the source regression suite,
    # but it is not part of the native render-llm product runtime.
    Remove-Item -LiteralPath (Join-Path $assistantRuntimeFull "dist\fake.js") -Force
    Remove-Item -LiteralPath (Join-Path $assistantRuntimeFull "dist\fake.js.map") -Force
    Copy-Item -LiteralPath (Join-Path $assistantSource "launch.cmd") -Destination $assistantRuntimeFull
    Copy-Item -LiteralPath (Join-Path $assistantSource "package.json") -Destination $assistantRuntimeFull
    Copy-Item -LiteralPath (Join-Path $assistantSource "package-lock.json") -Destination $assistantRuntimeFull
    Copy-Item -LiteralPath (Join-Path $assistantSource "prompts\render-llm-role.md") -Destination (Join-Path $assistantRuntimeFull "prompts")
    Copy-Item -LiteralPath (Join-Path $assistantSource "tools\run_render_inst.ps1") -Destination (Join-Path $assistantRuntimeFull "tools")
    & $npmCommand ci --omit=dev --ignore-scripts --no-audit --no-fund --prefix $assistantRuntimeFull
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & node (Join-Path $assistantSource "test\verify-runtime-package.mjs") $assistantRuntimeFull
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $openMpRuntime = Join-Path $repoRoot "Installer\Windows\vcomp140_x64.dll"
    if (Test-Path -LiteralPath $openMpRuntime -PathType Leaf) {
        Copy-Item -LiteralPath $openMpRuntime -Destination (Join-Path $runtimeDir "vcomp140.dll") -Force
    }

    if ($hasOidn -and $hasDxc) {
        & (Join-Path $PSScriptRoot "stage-native-runtime.ps1") `
            -Destination $runtimeDir -OidnBin $OidnBin -DxcBin $DxcBin
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }
    elseif ($RequireGpuRuntime -or -not $AllowGpuFallback) {
        throw "GPU runtime staging was required, but OidnBin and DxcBin were not supplied."
    }
    else {
        Write-Warning "Staging a diagnostic package without DXC/OIDN; Flame will use its fallback backend/denoiser."
    }

    # Audit shipped resources, not existing user projects, chats or render outputs.
    # A release archive must use a fresh build directory; an in-place rebuild must
    # preserve mutable Data and must not misclassify user PNGs as product assets.
    $resourceItems = @($resourceRoots | ForEach-Object {
        $resource = Join-Path $dataDestination $_
        Get-Item -LiteralPath $resource
        if (Test-Path -LiteralPath $resource -PathType Container) { Get-ChildItem -LiteralPath $resource -Recurse }
    })
    $rasterExtensions = @(".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp", ".tga", ".dds", ".hdr", ".exr")
    $stagedRasters = @($resourceItems | Where-Object {
        $_.Extension.ToLowerInvariant() -in $rasterExtensions
    })
    if ($stagedRasters.Count -ne 0) {
        throw "Static raster content leaked into native Data: $($stagedRasters[0].FullName)"
    }
    $retiredNamePattern = "(?i)minecraft|mine.?imator|mi-3|canon|armor|elytra|shield|sticker|shinytrim|splash|texture"
    $retiredPaths = @($resourceItems | Where-Object {
        $_.Name -match $retiredNamePattern
    })
    if ($retiredPaths.Count -ne 0) {
        throw "Retired content path leaked into native Data: $($retiredPaths[0].FullName)"
    }
    $stagedLegacy = Join-Path $dataDestination "legacy.midata"
    $expectedLegacy = Join-Path $repoRoot "tools\runtime-data\legacy.midata"
    if ((Get-FileHash -LiteralPath $stagedLegacy -Algorithm SHA256).Hash -ne `
        (Get-FileHash -LiteralPath $expectedLegacy -Algorithm SHA256).Hash) {
        throw "The staged legacy compatibility contract does not match the committed empty contract."
    }
    foreach ($retired in $retiredDataFolders) {
        if (Test-Path -LiteralPath (Join-Path $dataDestination $retired)) {
            throw "Old resource directory remains: $retired. Use a fresh build directory; existing data was not deleted."
        }
    }

    Write-Host "Staged native runtime at $runtimeDir (0 shipped raster assets, 0 retired resource paths; existing user Data preserved)."
}
