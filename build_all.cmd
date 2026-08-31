@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
set "SRC=%ROOT%native"
set "OUT=%ROOT%dist\bin"
set "OBJ=%ROOT%build\obj"

if not defined CREO_COMMON_FILES (
  echo ERROR: Set CREO_COMMON_FILES to the Creo Common Files directory.
  echo Example: set "CREO_COMMON_FILES=C:\Program Files\PTC\Creo 10.0.0.0\Common Files"
  exit /b 2
)

if not defined VSDEVCMD (
  for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
    set "VSDEVCMD=%%I\Common7\Tools\VsDevCmd.bat"
  )
)
if not exist "%VSDEVCMD%" (
  echo ERROR: Set VSDEVCMD to Visual Studio's VsDevCmd.bat.
  exit /b 3
)

set "PTC_ROOT=%CREO_COMMON_FILES%\protoolkit"
set "PTC_INC=%PTC_ROOT%\includes"
set "PTC_OBJ=%PTC_ROOT%\x86e_win64\obj"
if not exist "%PTC_INC%\ProToolkit.h" (
  echo ERROR: Pro/TOOLKIT headers were not found under %PTC_INC%.
  exit /b 4
)

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"
call "%VSDEVCMD%" -arch=amd64 -host_arch=amd64 >nul
if errorlevel 1 exit /b %errorlevel%

for %%F in ("%SRC%\creo_*.c") do (
  set "BASE=%%~nF"
  if /I not "!BASE!"=="creo_safe_resident_dll" (
    echo Building !BASE!.exe
    cl /nologo /utf-8 /c /O2 /GS /fp:precise /D_WSTDIO_DEFINED /DPRO_MACHINE=36 /DPRO_OS=4 /I"%PTC_INC%" "%%~fF" /Fo"%OBJ%\!BASE!.obj"
    if errorlevel 1 exit /b !errorlevel!
    link /nologo /out:"%OUT%\!BASE!.exe" /subsystem:console /debug:none /machine:amd64 "%OBJ%\!BASE!.obj" "%PTC_OBJ%\protoolkit_NU.lib" "%PTC_OBJ%\pt_asynchronous.lib" "%PTC_OBJ%\ucore.lib" "%PTC_OBJ%\udata.lib" libcmt.lib kernel32.lib user32.lib wsock32.lib advapi32.lib mpr.lib winspool.lib netapi32.lib psapi.lib gdi32.lib shell32.lib comdlg32.lib ole32.lib ws2_32.lib winmm.lib version.lib
    if errorlevel 1 exit /b !errorlevel!
  )
)

echo Building creo_safe_resident.dll
cl /nologo /utf-8 /c /O2 /GS /fp:precise /D_WSTDIO_DEFINED /DPRO_MACHINE=36 /DPRO_OS=4 /I"%PTC_INC%" "%SRC%\creo_safe_resident_dll.c" /Fo"%OBJ%\creo_safe_resident_dll.obj"
if errorlevel 1 exit /b %errorlevel%
link /nologo /out:"%OUT%\creo_safe_resident.dll" /dll /subsystem:console /debug:none /machine:amd64 "%OBJ%\creo_safe_resident_dll.obj" "%PTC_OBJ%\protk_dll_NU.lib" "%PTC_OBJ%\ucore.lib" "%PTC_OBJ%\udata.lib" libcmt.lib kernel32.lib user32.lib wsock32.lib advapi32.lib mpr.lib winspool.lib netapi32.lib psapi.lib gdi32.lib shell32.lib comdlg32.lib ole32.lib ws2_32.lib winmm.lib version.lib
if errorlevel 1 exit /b %errorlevel%

echo Build completed: %OUT%
exit /b 0
