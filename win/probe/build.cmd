@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
ml64 /nologo /c fsprobe_asm.asm || exit /b 1
cl /nologo /O2 /W4 fsprobe.c fsprobe_asm.obj /link /SUBSYSTEM:CONSOLE,6.01 || exit /b 1
fsprobe.exe
