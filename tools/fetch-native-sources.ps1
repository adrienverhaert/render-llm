param([string]$Destination = 'build/dependencies')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$destinationPath = [IO.Path]::GetFullPath((Join-Path $repo $Destination))
if (-not $destinationPath.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Dependency destination must be inside the repository.'
}
$manifestPath = Join-Path $PSScriptRoot 'native-sources.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schema -ne 1) { throw 'Unsupported source manifest.' }
$marker = Join-Path $destinationPath 'native-sources.json'
if (Test-Path -LiteralPath $destinationPath) {
    if (-not (Test-Path -LiteralPath $marker) -or (Get-FileHash -LiteralPath $marker).Hash -ne (Get-FileHash -LiteralPath $manifestPath).Hash) {
        throw 'Destination already exists without a matching completed installation; choose a fresh destination.'
    }
    foreach ($source in $manifest.sources) {
        if (-not (Test-Path -LiteralPath (Join-Path $destinationPath "$($source.name)/$($source.header)"))) { throw 'Dependency files are incomplete; use a fresh destination.' }
    }
    Write-Output 'Pinned native sources already present.'
    return
}
New-Item -ItemType Directory -Path $destinationPath | Out-Null
foreach ($source in $manifest.sources) {
    $archive = Join-Path $destinationPath $source.archive
    Invoke-WebRequest -Uri $source.url -OutFile $archive
    if ((Get-FileHash -LiteralPath $archive).Hash -ne $source.sha256) { throw "Source hash mismatch: $($source.name)" }
    $folder = Join-Path $destinationPath $source.name
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    & tar -xf $archive --strip-components=1 -C $folder
    if ($LASTEXITCODE -ne 0) { throw "Extraction failed: $($source.name)" }
    if (-not (Test-Path -LiteralPath (Join-Path $folder $source.header))) { throw "Header missing: $($source.name)" }
}
# The retained FFmpeg libraries target Windows x64: little endian with unaligned access.
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'runtime-data/avconfig.h') -Destination (Join-Path $destinationPath 'FFmpeg/ffmpeg-5.0/libavutil/avconfig.h')
Copy-Item -LiteralPath $manifestPath -Destination $marker
Write-Output "Verified and extracted $($manifest.sources.Count) pinned native source archives."
