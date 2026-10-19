@echo off
rem Compila y arma dist\camara-vs-<version>.zip para publicar en Releases.
setlocal
cd /d "%~dp0"
call "%~dp0build.bat" || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$v = (Select-String -Path src\camara_vs.c -Pattern '#define VERSION \"(.+)\"').Matches[0].Groups[1].Value;" ^
  "$tmp = Join-Path dist 'paquete'; Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $tmp | Out-Null;" ^
  "Copy-Item dist\camara_vs.dll, dist\inyectar.exe, instalador\* $tmp;" ^
  "$zip = Join-Path dist ('camara-vs-' + $v + '.zip'); Remove-Item $zip -ErrorAction SilentlyContinue;" ^
  "Compress-Archive -Path (Join-Path $tmp '*') -DestinationPath $zip; Remove-Item $tmp -Recurse -Force; Write-Host ('listo: ' + $zip)"
