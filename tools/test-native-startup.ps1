param(
    [string]$Runtime='build/native-startup-asan/Release',
    [string]$Evidence='build/startup-diagnostic',
    [ValidateRange(1,20)][int]$Runs=8
)
$ErrorActionPreference='Stop'
$runtimePath=(Resolve-Path -LiteralPath $Runtime).Path
$exe=Join-Path $runtimePath 'render-llm.exe'
if(-not (Test-Path -LiteralPath $exe -PathType Leaf)){throw 'Native executable missing'}
$evidencePath=[IO.Path]::GetFullPath($Evidence)
New-Item -ItemType Directory -Path $evidencePath -Force | Out-Null
$keys=@('RENDER_LLM_CAPTURE_UI','RENDER_LLM_CAPTURE_SETTLE_MS','RENDER_LLM_STARTUP_TRACE','ASAN_SAVE_DUMPS','ASAN_OPTIONS')
$previous=@{};foreach($key in $keys){$previous[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$results=@()
$success=$false
Add-Type -AssemblyName System.Drawing
try {
    $env:RENDER_LLM_CAPTURE_UI='1';$env:RENDER_LLM_CAPTURE_SETTLE_MS='1000';$env:RENDER_LLM_STARTUP_TRACE='1'
    $env:ASAN_OPTIONS='halt_on_error=1:abort_on_error=1'
    foreach($run in 1..$Runs){
        $env:ASAN_SAVE_DUMPS=Join-Path $evidencePath "startup-$run.dmp"
        $timer=[Diagnostics.Stopwatch]::StartNew();$started=Get-Date
        $p=Start-Process -FilePath $exe -WorkingDirectory $runtimePath -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $evidencePath "startup-$run.stderr.txt") -RedirectStandardOutput (Join-Path $evidencePath "startup-$run.stdout.txt")
        $finished=$p.WaitForExit(45000)
        if(-not $finished){Stop-Process -Id $p.Id;$p.WaitForExit()}
        $results+=@{run=$run;exitCode=$p.ExitCode;timedOut=(-not $finished);elapsedMs=$timer.ElapsedMilliseconds}
        if(-not $finished -or $p.ExitCode -ne 0){
            # Keep symbols matching the failing binary before another build replaces them.
            Copy-Item -LiteralPath $exe,(Join-Path $runtimePath 'render-llm.pdb') -Destination $evidencePath
            throw "Startup $run failed: exit=$($p.ExitCode), timeout=$(-not $finished)"
        }
        $capture=Get-Item -LiteralPath (Join-Path $runtimePath 'Data/renderllm-ui.png')
        if($capture.LastWriteTime -lt $started){throw "Startup $run returned a stale capture"}
        $img=[Drawing.Image]::FromFile($capture.FullName)
        try {if($img.Width -lt 960 -or $img.Height -lt 640){throw 'Unexpected capture dimensions'}}finally{$img.Dispose()}
        Copy-Item -LiteralPath $capture.FullName -Destination (Join-Path $evidencePath "startup-$run.png")
        Write-Output "PASS startup $run ($($timer.ElapsedMilliseconds) ms, capture decoded)"
    }
    $success=$true
} finally {
    @{success=$success;exeSha256=(Get-FileHash -LiteralPath $exe).Hash;executedAt=(Get-Date).ToUniversalTime().ToString('o');checks=$results} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidencePath 'result.json')
    foreach($key in $keys){if($null -eq $previous[$key]){Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue}else{[Environment]::SetEnvironmentVariable($key,$previous[$key],'Process')}}
}
