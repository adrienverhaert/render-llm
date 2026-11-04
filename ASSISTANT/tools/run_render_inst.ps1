# ASSISTANT\tools\run_render_inst.ps1
# Corre un op-script headless en una instancia dedicada de render-llm.
# Diferencias con HEADLESS\tools\run_headless_inst.ps1:
#   - la instancia es obligatoria (nunca hay default al install del usuario),
#   - deja el pid en -PidFile para que el cerebro pueda cancelar con precision,
#   - solo mata procesos cuyo Path sea EXACTAMENTE el exe de ESTA instancia.
param(
    [Parameter(Mandatory=$true)][string]$OpScript,
    [Parameter(Mandatory=$true)][string]$InstanceDir,
    [int]$TimeoutSec = 600,
    [string]$ExeName = "render-llm.exe",
    [string]$PidFile = ""
)
$exe     = Join-Path $InstanceDir $ExeName
$trigger = Join-Path $InstanceDir "Data\headless.json"
$result  = Join-Path $InstanceDir "Data\headless_result.json"

if (-not (Test-Path $exe))      { Write-Error "no existe el exe: $exe"; exit 2 }
if (-not (Test-Path $OpScript)) { Write-Error "op-script no existe: $OpScript"; exit 2 }

foreach ($f in @($result, $trigger, ($trigger + ".consumed"))) {
    if (Test-Path $f) { Remove-Item $f -Force -ErrorAction SilentlyContinue }
}
Copy-Item $OpScript $trigger -Force

$p = Start-Process -FilePath $exe -WorkingDirectory $InstanceDir -WindowStyle Hidden -PassThru
if ($PidFile -ne "") { $p.Id | Out-File $PidFile -Encoding ascii -Force }

$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
    if (Test-Path $result) { Start-Sleep -Milliseconds 1500; break }
    if ($p.HasExited) {
        # Mismo fix que run_headless.ps1 (260c3c4b): con el lanzador invisible el
        # proceso a veces SALE AL INSTANTE porque se re-lanza como hijo. Esperar
        # al ARTEFACTO, no al proceso.
        $alive = Get-Process -ErrorAction SilentlyContinue |
                 Where-Object { $_.Path -and ($_.Path -eq $exe) }
        if (-not $alive) {
            Start-Sleep -Milliseconds 2000
            if (-not (Test-Path $result)) { break }
        }
    }
    Start-Sleep -Milliseconds 1000
}

if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
Get-Process -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and ($_.Path -eq $exe) } | Stop-Process -Force -ErrorAction SilentlyContinue

if (Test-Path $trigger) { Remove-Item $trigger -Force -ErrorAction SilentlyContinue }

if (Test-Path $result) {
    Get-Content $result -Raw
    exit 0
} else {
    Write-Error "sin headless_result.json (timeout o crash; ver $InstanceDir\Data\log.txt)"
    exit 1
}
