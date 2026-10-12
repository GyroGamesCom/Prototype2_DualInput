@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%~dp0"
cl /nologo /MT /O2 /GS- /W3 /D_CRT_SECURE_NO_WARNINGS /LD xinput1_3.c /Fe:xinput1_3.dll /link /DEF:xinput1_3.def user32.lib kernel32.lib dinput8.lib dxguid.lib || exit /b 1
if not exist ..\mod mkdir ..\mod
copy /y xinput1_3.dll ..\mod\xinput1_3.dll >nul || exit /b 1
dumpbin /exports ..\mod\xinput1_3.dll
del xinput1_3.obj xinput1_3.exp xinput1_3.lib xinput1_3.dll 2>nul
