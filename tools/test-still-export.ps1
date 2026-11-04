param([string]$Runtime='build/native-hierarchy-clean-20260915/Release',
    [string]$Evidence='docs/evidence/ui/output-still',
    [int]$Width=1280,[int]$Height=720,[int]$Samples=16,[switch]$Custom,[switch]$Compact)
$ErrorActionPreference='Stop'
$runtimePath=(Resolve-Path -LiteralPath $Runtime).Path
$evidencePath=[IO.Path]::GetFullPath($Evidence)
if(Test-Path -LiteralPath $evidencePath){throw 'Use a fresh evidence directory.'}
New-Item -ItemType Directory -Path $evidencePath | Out-Null
$keys=@('RENDER_LLM_CAPTURE_UI','RENDER_LLM_CAPTURE_SCENE','RENDER_LLM_CAPTURE_RENDER_UI','RENDER_LLM_RUN_EXPORT_UI','RENDER_LLM_TEST_EXPORT_REQUEST','RENDER_LLM_TEST_EXPORT_COMPACT')
$previous=@{};foreach($key in $keys){$previous[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$fixtureName="still-$([guid]::NewGuid().ToString('N')).renderllm"
$fixture=Join-Path $runtimePath "Data/renderllm-projects/$fixtureName"
try {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../docs/evidence/ui/studio-motion.renderllm') -Destination $fixture
    $env:RENDER_LLM_CAPTURE_UI='1';$env:RENDER_LLM_CAPTURE_SCENE=$fixtureName
    $env:RENDER_LLM_CAPTURE_RENDER_UI='1';$env:RENDER_LLM_RUN_EXPORT_UI='1'
    if($Compact){$env:RENDER_LLM_TEST_EXPORT_COMPACT='1'}else{Remove-Item Env:RENDER_LLM_TEST_EXPORT_COMPACT -ErrorAction SilentlyContinue}
    if($Custom){$env:RENDER_LLM_TEST_EXPORT_REQUEST=@{width=$Width;height=$Height;samples=$Samples}|ConvertTo-Json -Compress}else{Remove-Item Env:RENDER_LLM_TEST_EXPORT_REQUEST -ErrorAction SilentlyContinue}
    $started=Get-Date
    $p=Start-Process -FilePath (Join-Path $runtimePath 'render-llm.exe') -WorkingDirectory $runtimePath -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $evidencePath 'stdout.log') -RedirectStandardError (Join-Path $evidencePath 'stderr.log')
    if(!$p.WaitForExit(90000)){Stop-Process -Id $p.Id;throw 'Still export timed out'}
    $report=Get-Item -LiteralPath (Join-Path $runtimePath 'Data/studio-export-complete.json')
    if($report.LastWriteTime -lt $started){throw 'Stale still report'}
    Copy-Item -LiteralPath $report.FullName -Destination $evidencePath
    $native=Get-Content -LiteralPath $report.FullName -Raw | ConvertFrom-Json
    if($p.ExitCode -ne 0 -or !$native.passed -or !$native.scene_unchanged){throw 'Native still failed'}
    $result=$native.result
    if($result.width -ne $Width -or $result.height -ne $Height -or $result.samples -ne $Samples){throw 'Still settings were not preserved'}
    $dataRoot=[IO.Path]::GetFullPath((Join-Path $runtimePath 'Data/assistant'))
    $imagePath=[IO.Path]::GetFullPath((Join-Path $dataRoot $result.image))
    if(!$imagePath.StartsWith($dataRoot+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Still escaped data root'}
    if((Get-FileHash -LiteralPath $imagePath).Hash -ne $result.image_sha256){throw 'Still hash mismatch'}
    Copy-Item -LiteralPath $imagePath -Destination (Join-Path $evidencePath 'image.png')
    Copy-Item -LiteralPath (Join-Path $runtimePath 'Data/studio-output-result.png') -Destination (Join-Path $evidencePath 'output.png')
    $probe=& ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of json $imagePath
    if($LASTEXITCODE -ne 0){throw 'Still probe failed'}
    $stream=($probe|ConvertFrom-Json).streams[0]
    if($stream.width -ne $Width -or $stream.height -ne $Height){throw 'Decoded still dimensions mismatch'}
    $rgb=& ffmpeg -v error -i $imagePath -pix_fmt rgb24 -f hash -hash sha256 - 2> (Join-Path $evidencePath 'decode.log')
    if($LASTEXITCODE -ne 0){throw 'Full still RGB decode failed'}
    @{passed=$true;exeSha256=(Get-FileHash (Join-Path $runtimePath 'render-llm.exe')).Hash;default_settings=(-not $Custom);width=$Width;height=$Height;samples=$Samples;scene_unchanged=$true;rgb_sha256=($rgb -replace '^SHA256=','');render=$result}|ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $evidencePath 'result.json')
    Write-Output "PASS native still: $Width x $Height / $Samples samples; full RGB decode."
} finally {
    if(Test-Path -LiteralPath $fixture){Remove-Item -LiteralPath $fixture}
    foreach($key in $keys){if($null -eq $previous[$key]){Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue}else{[Environment]::SetEnvironmentVariable($key,$previous[$key],'Process')}}
}
