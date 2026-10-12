@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%~dp0"
rem winmm_exports.h and winmm.def are generated (see gen_winmm_exports.py); regenerate with:
rem   dumpbin /exports C:\Windows\SysWOW64\winmm.dll > winmm_dump.txt  &&  python -I gen_winmm_exports.py winmm_dump.txt
cl /nologo /MT /O2 /GS- /W3 /D_CRT_SECURE_NO_WARNINGS /LD winmm.c /Fe:winmm.dll /link /DEF:winmm.def user32.lib kernel32.lib || exit /b 1
if not exist ..\..\mod_hires mkdir ..\..\mod_hires
copy /y winmm.dll ..\..\mod_hires\winmm.dll >nul || exit /b 1
echo --- built:
dumpbin /exports ..\..\mod_hires\winmm.dll | findstr /c:"number of functions" /c:"number of names" /c:"ordinal base"
echo --- real SysWOW64 winmm:
dumpbin /exports C:\Windows\SysWOW64\winmm.dll | findstr /c:"number of functions" /c:"number of names" /c:"ordinal base"
del winmm.obj winmm.exp winmm.lib winmm.dll 2>nul
