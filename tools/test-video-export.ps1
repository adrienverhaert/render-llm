param([string]$Runtime='build/native-final-audit/Release',[string]$Evidence='build/checks/video',
    [ValidateSet('mp4','png-sequence')][string]$Format='mp4',
    [int]$Width=384,[int]$Height=216,[int]$Samples=16,[int]$Frames=96,[int]$Fps=24,[switch]$Compact,
    [string]$Scene=(Join-Path $PSScriptRoot '../examples/studio-bounce.renderllm'))
$ErrorActionPreference='Stop'
$runtimePath=(Resolve-Path -LiteralPath $Runtime).Path
$evidencePath=[IO.Path]::GetFullPath($Evidence)
if(Test-Path -LiteralPath $evidencePath){throw 'Use a fresh evidence directory.'}
New-Item -ItemType Directory -Path $evidencePath | Out-Null
$keys=@('RENDER_LLM_CAPTURE_UI','RENDER_LLM_CAPTURE_SCENE','RENDER_LLM_CAPTURE_VIDEO_UI','RENDER_LLM_RUN_EXPORT_UI','RENDER_LLM_TEST_EXPORT_FORMAT','RENDER_LLM_TEST_EXPORT_REQUEST','RENDER_LLM_TEST_EXPORT_COMPACT')
$previous=@{};foreach($key in $keys){$previous[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$fixtureName="video-$([guid]::NewGuid().ToString('N')).renderllm"
$fixture=Join-Path $runtimePath "Data/renderllm-projects/$fixtureName"
try {
    Copy-Item -LiteralPath  $Scene -Destination $fixture
    $env:RENDER_LLM_CAPTURE_UI='1';$env:RENDER_LLM_CAPTURE_SCENE=$fixtureName
    $env:RENDER_LLM_CAPTURE_VIDEO_UI='1';$env:RENDER_LLM_RUN_EXPORT_UI='1'
    $env:RENDER_LLM_TEST_EXPORT_FORMAT=$Format
    if($Compact){$env:RENDER_LLM_TEST_EXPORT_COMPACT='1'}else{Remove-Item Env:RENDER_LLM_TEST_EXPORT_COMPACT -ErrorAction SilentlyContinue}
    $env:RENDER_LLM_TEST_EXPORT_REQUEST=@{width=$Width;height=$Height;samples=$Samples;frames=$Frames;fps=$Fps}|ConvertTo-Json -Compress
    $started=Get-Date
    $p=Start-Process -FilePath (Join-Path $runtimePath 'render-llm.exe') -WorkingDirectory $runtimePath -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $evidencePath 'stdout.log') -RedirectStandardError (Join-Path $evidencePath 'stderr.log')
    if(!$p.WaitForExit(120000)){Stop-Process -Id $p.Id;throw 'Native video export exceeded 120 seconds.'}
    if($p.ExitCode -ne 0){throw "Native video export exited $($p.ExitCode)."}
    $nativeReport=Get-Item -LiteralPath (Join-Path $runtimePath 'Data/studio-export-complete.json')
    if($nativeReport.LastWriteTime -lt $started){throw 'Stale native export report'}
    $nativeResult=Get-Content -LiteralPath $nativeReport.FullName -Raw | ConvertFrom-Json
    if(!$nativeResult.passed -or !$nativeResult.scene_unchanged){throw 'Native export did not preserve the scene'}
    $result=$nativeResult.result
    if($result.width -ne $Width -or $result.height -ne $Height -or $result.samples -ne $Samples -or $result.frames -ne $Frames -or $result.fps -ne $Fps){throw 'Output settings did not reach the native renderer'}
    Copy-Item -LiteralPath $nativeReport.FullName -Destination $evidencePath
    if($Format -eq 'png-sequence') {
        $result=$nativeResult.result
        if($result.format -ne $Format -or $result.duration_s -ne ($Frames/$Fps)){throw 'Sequence metadata mismatch'}
        $dataRoot=[IO.Path]::GetFullPath((Join-Path $runtimePath 'Data/assistant'))
        $sequence=[IO.Path]::GetFullPath((Join-Path $dataRoot $result.sequence))
        if(!$sequence.StartsWith($dataRoot+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Sequence escaped data root'}
        $frameFiles=@(Get-ChildItem -LiteralPath $sequence -File)
        if($frameFiles.Count -ne $Frames -or @($result.images).Count -ne $Frames){throw 'Sequence complete PNG count mismatch'}
        foreach($entry in $result.images){$frame=Join-Path $dataRoot $entry.image;if((Get-FileHash -LiteralPath $frame).Hash -ne $entry.image_sha256){throw 'PNG hash mismatch'}}
        Copy-Item -LiteralPath $sequence -Destination (Join-Path $evidencePath 'frames') -Recurse
        Copy-Item -LiteralPath (Join-Path (Split-Path $sequence -Parent) 'manifest.json') -Destination $evidencePath
        Copy-Item -LiteralPath (Join-Path $runtimePath 'Data/studio-output-result.png') -Destination (Join-Path $evidencePath 'output.png')
        & ffmpeg -hide_banner -v error -framerate $Fps -i (Join-Path $sequence 'frame-%06d.png') -pix_fmt rgb24 -f framehash -hash sha256 - 1> (Join-Path $evidencePath 'framehash.txt') 2> (Join-Path $evidencePath 'decode.log')
        if($LASTEXITCODE -ne 0){throw 'PNG sequence full decode failed'}
        $decoded=@(Get-Content (Join-Path $evidencePath 'framehash.txt') | Where-Object {$_ -and -not $_.StartsWith('#')})
        if($decoded.Count -ne $Frames){throw 'Decoded PNG frame count mismatch'}
        @{passed=$true;exeSha256=(Get-FileHash (Join-Path $runtimePath 'render-llm.exe')).Hash;format=$Format;decoded_frames=$Frames;fps=$Fps;seconds=($Frames/$Fps);width=$Width;height=$Height;samples=$Samples;scene_unchanged=$true}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $evidencePath 'result.json')
        Write-Output "PASS native PNG sequence: fully decoded $Frames frames / $Fps fps."
        return
    }
    $videos=@(Get-ChildItem -LiteralPath (Join-Path $runtimePath 'Data/assistant/video') -Recurse -File -Filter video.mp4 | Where-Object LastWriteTime -GE $started)
    if($videos.Count -ne 1 -or $videos[0].Length -le 0){throw 'Expected exactly one fresh, nonempty video.'}
    $video=$videos[0]
    Copy-Item -LiteralPath $video.FullName -Destination (Join-Path $evidencePath 'video.mp4')
    $manifest=Join-Path $video.DirectoryName 'manifest.json'
    Copy-Item -LiteralPath $manifest -Destination $evidencePath
    Copy-Item -LiteralPath (Join-Path $runtimePath 'Data/studio-output-result.png') -Destination (Join-Path $evidencePath 'output.png')
    $probe=& ffprobe -v error -select_streams v:0 -count_frames -show_entries stream=codec_name,width,height,r_frame_rate,nb_read_frames,duration -of json $video.FullName
    if($LASTEXITCODE -ne 0){throw 'Video probe failed.'}
    $probe | Set-Content -LiteralPath (Join-Path $evidencePath 'probe.json')
    $stream=($probe | ConvertFrom-Json).streams[0]
    if($stream.nb_read_frames -ne $Frames -or $stream.r_frame_rate -ne "$Fps/1" -or [Math]::Abs([double]$stream.duration-($Frames/$Fps)) -gt 0.00001 -or $stream.width -ne $Width -or $stream.height -ne $Height){throw 'Decoded video dimensions or timing do not match the request.'}
    & ffmpeg -hide_banner -v error -i $video.FullName -f null - 2> (Join-Path $evidencePath 'decode.log')
    if($LASTEXITCODE -ne 0){throw 'Full video decode failed.'}
    @{passed=$true;exeSha256=(Get-FileHash (Join-Path $runtimePath 'render-llm.exe')).Hash;videoSha256=(Get-FileHash $video.FullName).Hash;bytes=$video.Length;decoded_frames=$Frames;fps=$Fps;seconds=($Frames/$Fps);width=$Width;height=$Height;samples=$Samples} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidencePath 'result.json')
    Write-Output "PASS native output: decoded $Frames frames / $Fps fps."
} finally {
    if(Test-Path -LiteralPath $fixture){Remove-Item -LiteralPath $fixture}
    foreach($key in $keys){if($null -eq $previous[$key]){Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue}else{[Environment]::SetEnvironmentVariable($key,$previous[$key],'Process')}}
}
