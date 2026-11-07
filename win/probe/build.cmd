@echo off
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
ml64 /nologo /c fsprobe_asm.asm || exit /b 1
cl /nologo /O2 /W4 fsprobe.c fsprobe_asm.obj /link /SUBSYSTEM:CONSOLE,6.01 || exit /b 1
fsprobe.exe
