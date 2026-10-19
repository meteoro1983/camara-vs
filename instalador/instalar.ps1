# Camara VS - instala o desinstala el modulo en el kload.cfg de Kitserver 5.
# Uso: instalar.bat / desinstalar.bat (desde la carpeta kitserver del juego).
param([switch]$Quitar)
$ErrorActionPreference = 'Stop'
$aca = Split-Path -Parent $MyInvocation.MyCommand.Path
$cfg = Join-Path $aca 'kload.cfg'
$linea = '"kitserver\camara_vs.dll"'

function Salir($msg, $codigo = 0) { Write-Host $msg; exit $codigo }

if (-not (Test-Path $cfg)) {
    Salir "No encuentro kload.cfg en $aca.`nCopia los archivos de Camara VS dentro de la carpeta 'kitserver' del juego y volve a ejecutar." 1
}
if (-not $Quitar -and -not (Test-Path (Join-Path $aca 'camara_vs.dll'))) {
    Salir "Falta camara_vs.dll en $aca." 1
}

$crudo = [IO.File]::ReadAllText($cfg)
$nl = if ($crudo.Contains("`r`n")) { "`r`n" } else { "`n" }   # se respeta el fin de linea original
$texto = $crudo -split "`r?`n"
if ($texto.Count -gt 0 -and $texto[-1] -eq '') { $texto = $texto[0..($texto.Count - 2)] }
# Lineas DLL.N activas (sin comentar), en orden.
$dlls = @()
foreach ($l in $texto) {
    if ($l -match '^\s*DLL\.(\d+)\s*=\s*(.+?)\s*$') { $dlls += [pscustomobject]@{ N = [int]$Matches[1]; Valor = $Matches[2] } }
}
$dlls = @($dlls | Sort-Object N)
$ya = @($dlls | Where-Object { $_.Valor -match 'camara_vs\.dll' }).Count -gt 0

if ($Quitar) {
    if (-not $ya) { Salir 'Camara VS no estaba instalada en kload.cfg.' }
    $dlls = @($dlls | Where-Object { $_.Valor -notmatch 'camara_vs\.dll' })
} else {
    if ($ya) { Salir 'Camara VS ya estaba instalada. Para ajustarla edita camara_vs.ini.' }
    $dlls += [pscustomobject]@{ N = 9999; Valor = $linea }
}

Copy-Item $cfg "$cfg.antes-de-camara-vs" -Force
# Reescribe el archivo: las lineas DLL.N quedan numeradas 0..n-1 donde estaba la primera.
$salida = New-Object System.Collections.Generic.List[string]
$puestas = $false
foreach ($l in $texto) {
    if ($l -match '^\s*DLL\.num\s*=') { $salida.Add("DLL.num = $($dlls.Count)"); continue }
    if ($l -match '^\s*DLL\.\d+\s*=') {
        if (-not $puestas) {
            for ($i = 0; $i -lt $dlls.Count; $i++) { $salida.Add("DLL.$i = $($dlls[$i].Valor)") }
            $puestas = $true
        }
        continue
    }
    $salida.Add($l)
}
if (-not $puestas) { foreach ($i in 0..($dlls.Count - 1)) { $salida.Add("DLL.$i = $($dlls[$i].Valor)") } }
[IO.File]::WriteAllText($cfg, ($salida -join $nl) + $nl)

if ($Quitar) { Salir "Camara VS desinstalada (respaldo: kload.cfg.antes-de-camara-vs)." }
Salir "Camara VS instalada como DLL.$($dlls.Count - 1) (respaldo: kload.cfg.antes-de-camara-vs).`nEn el partido: F9 activa/desactiva. Ajustes en camara_vs.ini."
