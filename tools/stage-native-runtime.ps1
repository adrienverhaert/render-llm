param(
    [Parameter(Mandatory = $true)]
    [string]$Destination,
    [Parameter(Mandatory = $true)]
    [string]$OidnBin,
    [Parameter(Mandatory = $true)]
    [string]$DxcBin,
    [string]$Manifest = (Join-Path $PSScriptRoot "native-runtime.json")
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Manifest -PathType Leaf)) {
    throw "Native runtime manifest not found: $Manifest"
}

$runtime = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
if ($runtime.schema -ne 1) {
    throw "Unsupported native runtime manifest schema: $($runtime.schema)"
}

$sources = @{
    oidn = $OidnBin
    dxc = $DxcBin
}

$verified = @()
foreach ($componentName in @("oidn", "dxc")) {
    $sourceDir = $sources[$componentName]
    if (-not (Test-Path -LiteralPath $sourceDir -PathType Container)) {
        throw "$componentName runtime directory not found: $sourceDir"
    }

    $component = $runtime.components.$componentName
    foreach ($fileProperty in $component.files.PSObject.Properties) {
        $sourceFile = Join-Path $sourceDir $fileProperty.Name
        if (-not (Test-Path -LiteralPath $sourceFile -PathType Leaf)) {
            throw "Required $componentName runtime file not found: $sourceFile"
        }

        $actualHash = (Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash
        if ($actualHash -ne $fileProperty.Value) {
            throw "SHA-256 mismatch for $sourceFile. Expected $($fileProperty.Value), got $actualHash."
        }
        $verified += $sourceFile
    }
}

New-Item -ItemType Directory -Force -Path $Destination | Out-Null
foreach ($sourceFile in $verified) {
    Copy-Item -LiteralPath $sourceFile -Destination (Join-Path $Destination (Split-Path $sourceFile -Leaf)) -Force
}

Write-Host "Verified and staged $($verified.Count) native runtime files at $Destination"
