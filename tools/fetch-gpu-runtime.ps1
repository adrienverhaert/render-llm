param([string]$Destination = 'build/gpu-runtime')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$destinationPath = [IO.Path]::GetFullPath((Join-Path $repo $Destination))
if (-not $destinationPath.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'GPU input destination must be inside the repository.'
}
$manifestPath = Join-Path $PSScriptRoot 'native-runtime.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schema -ne 1) { throw 'Unsupported GPU runtime manifest.' }
New-Item -ItemType Directory -Path $destinationPath -Force | Out-Null
$bins = @{}
foreach ($component in $manifest.components.PSObject.Properties) {
    $name = $component.Name
    $archive = Join-Path $destinationPath "$name.zip"
    if (-not (Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest -Uri $component.Value.url -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive).Hash -ne $component.Value.archive_sha256) { throw "GPU archive hash mismatch: $name" }
    $folder = Join-Path $destinationPath $name
    if (-not (Test-Path -LiteralPath $folder)) { Expand-Archive -LiteralPath $archive -DestinationPath $folder }
    $bins[$name] = Join-Path $folder $component.Value.bin
}
& (Join-Path $PSScriptRoot 'stage-native-runtime.ps1') -Destination (Join-Path $destinationPath 'verified') `
    -OidnBin $bins['oidn'] -DxcBin $bins['dxc'] -Manifest $manifestPath
Write-Output 'GPU inputs ready. Use build/gpu-runtime/verified for both OidnBin and DxcBin (or your selected destination).'
