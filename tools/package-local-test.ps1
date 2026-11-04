param([string]$Runtime='build/native/Release',[string]$Destination='build/local-test-candidate')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$runtimePath=(Resolve-Path -LiteralPath $Runtime).Path
$destinationPath=[IO.Path]::GetFullPath($Destination)
$buildRoot=[IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\')+'\'
if(!$destinationPath.StartsWith($buildRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Destination must be a new directory within repository/build.'}
if(Test-Path -LiteralPath $destinationPath){throw 'Destination exists; choose a fresh directory. Existing builds are preserved.'}
$stage=Join-Path $destinationPath 'render-llm';$extracted=Join-Path $destinationPath 'verified-extraction'
New-Item -ItemType Directory -Path (Join-Path $stage 'Data/renderllm-projects') -Force | Out-Null
# Explicit immutable resources only. Never copy session, chats, credentials or user projects.
& (Join-Path $PSScriptRoot 'stage-native-runtime.ps1') -Destination $stage -OidnBin $runtimePath -DxcBin $runtimePath
foreach($name in @('render-llm.exe','vcomp140.dll')) {
    $file=Get-Item -LiteralPath (Join-Path $runtimePath $name)
    if($file.PSIsContainer -or $file.Length -le 0){throw "Missing runtime file: $name"}
    Copy-Item -LiteralPath $file.FullName -Destination $stage
}
foreach($name in @('Fonts','Languages','Libraries','Render','languages.midata','legacy.midata')) {
    Copy-Item -LiteralPath (Join-Path $runtimePath "Data/$name") -Destination (Join-Path $stage 'Data') -Recurse
}
if (Test-Path -LiteralPath (Join-Path $runtimePath 'Qt5Core.dll')) {
    foreach ($name in @('Qt5Core.dll', 'Qt5Gui.dll', 'Qt5Widgets.dll', 'Qt5Network.dll')) {
        Copy-Item -LiteralPath (Join-Path $runtimePath $name) -Destination $stage
    }
    New-Item -ItemType Directory -Path (Join-Path $stage 'platforms') | Out-Null
    Copy-Item -LiteralPath (Join-Path $runtimePath 'platforms/qwindows.dll') -Destination (Join-Path $stage 'platforms')
}
$assistant=Join-Path $runtimePath 'ASSISTANT'
$links=@(Get-ChildItem -LiteralPath $assistant -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint })
if($links.Count){throw 'Assistant runtime contains a reparse point; inspect before packaging.'}
Copy-Item -LiteralPath $assistant -Destination $stage -Recurse
Copy-Item -LiteralPath (Join-Path $repo 'examples/studio-bounce.renderllm') -Destination (Join-Path $stage 'Data/renderllm-projects/studio-bounce.renderllm')
Copy-Item -LiteralPath (Join-Path $repo 'LICENSING.md') -Destination $stage
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $stage
Copy-Item -LiteralPath (Join-Path $repo 'docs/QUICKSTART.md') -Destination (Join-Path $stage 'README.md')
Copy-Item -LiteralPath (Join-Path $repo 'Installer/Windows/license.txt') -Destination (Join-Path $stage 'UPSTREAM-NOTICE.txt')
& node (Join-Path $repo 'ASSISTANT/test/verify-runtime-package.mjs') (Join-Path $stage 'ASSISTANT')
if($LASTEXITCODE -ne 0){throw 'Staged agent verification failed.'}
$files=@(Get-ChildItem -LiteralPath $stage -Recurse -File -Force | ForEach-Object {
    @{path=$_.FullName.Substring($stage.Length+1).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
})
@{purpose='local test candidate; not a public release';prerequisites=@('Windows x64','Microsoft Visual C++ x64 runtime','Node.js 22+','Signed-in Codex CLI for native chat');files=$files} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'BUILD-MANIFEST.json')
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive=Join-Path $destinationPath 'render-llm-local-test.zip'
[IO.Compression.ZipFile]::CreateFromDirectory($stage,$archive,[IO.Compression.CompressionLevel]::Optimal,$false)
[IO.Compression.ZipFile]::ExtractToDirectory($archive,$extracted)
foreach($entry in $files) {
    $file=Get-Item -LiteralPath (Join-Path $extracted $entry.path)
    if($file.Length -ne $entry.bytes -or (Get-FileHash -LiteralPath $file.FullName).Hash -ne $entry.sha256){throw "Extracted file mismatch: $($entry.path)"}
}
if(Test-Path -LiteralPath (Join-Path $extracted 'Data/assistant')){throw 'Mutable agent state leaked into archive.'}
@{archive=$archive;archive_bytes=(Get-Item $archive).Length;archive_sha256=(Get-FileHash $archive).Hash;verified_files=$files.Count;extracted_runtime=$extracted;exe_sha256=(Get-FileHash (Join-Path $extracted 'render-llm.exe')).Hash;runtime_executed=$false} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destinationPath 'package-check.json')
Write-Output "Archive extracted and verified: $($files.Count) files. Runtime execution remains a separate check: $extracted"
