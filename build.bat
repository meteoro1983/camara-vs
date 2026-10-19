@echo off
rem Compila camara_vs.dll (32 bits) con Visual Studio Build Tools (cualquier version con C++).
setlocal
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set VS=& for /f "usebackq tokens=*" %%i in (`call %VSWHERE% -latest -products * -property installationPath`) do set VS=%%i
if not defined VS (echo No encontre Visual Studio Build Tools & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>nul || exit /b 1
cd /d "%~dp0"
if not exist dist mkdir dist
cl /nologo /O2 /W3 /MT /LD /D_CRT_SECURE_NO_WARNINGS src\camara_vs.c /Fodist\ /Fedist\camara_vs.dll /link /DLL user32.lib kernel32.lib || exit /b 1
cl /nologo /O2 /W3 /MT /D_CRT_SECURE_NO_WARNINGS src\inyectar.c /Fodist\ /Fedist\inyectar.exe || exit /b 1
del dist\*.obj dist\*.exp dist\*.lib 2>nul
echo listo: dist\camara_vs.dll
