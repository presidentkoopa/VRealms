@echo off
rem Build the DBASE100 dump tool (and the reader tests) with MSVC, then run the
rem dump against the retail DATA directory if one is given.
rem
rem   tools\dbase100_test\build.cmd "E:\path\to\retail\DATA"
rem
rem Builds into build-dxr\dbase100_test, never beside the sources (object files
rem next to the sources break the engine build with a PDB conflict).
setlocal
set HERE=%~dp0
set SRC=%HERE%..\..\src\roth\
set OUT=%HERE%..\..\build-dxr\dbase100_test
if not exist "%OUT%" mkdir "%OUT%"

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

set CORE="%SRC%roth_dbase100.cpp" "%SRC%roth_dbase100_install.cpp" "%SRC%roth_install.cpp" "%SRC%roth_log.cpp"

cl /nologo /std:c++17 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Fo:"%OUT%\\" /Fd:"%OUT%\dump.pdb" ^
   /Fe:"%OUT%\dbase100_dump.exe" "%HERE%dbase100_dump.cpp" %CORE%
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Fo:"%OUT%\\" /Fd:"%OUT%\test.pdb" ^
   /Fe:"%OUT%\dbase100_test.exe" "%HERE%dbase100_test.cpp" %CORE%
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Fo:"%OUT%\\" /Fd:"%OUT%\inv.pdb" ^
   /Fe:"%OUT%\inventory_test.exe" "%HERE%inventory_test.cpp" "%SRC%roth_inventory.cpp" %CORE%
if errorlevel 1 exit /b 1

"%OUT%\dbase100_test.exe" "%TEMP%"
if errorlevel 1 exit /b 1
"%OUT%\inventory_test.exe"
if errorlevel 1 exit /b 1

if "%~1"=="" (
	echo.
	echo Usage: build.cmd "path\to\retail\DATA"   ^(the folder with ROTH.RES and DBASE100.DAT^)
	exit /b 0
)
"%OUT%\dbase100_dump.exe" %1 --all > "%OUT%\dbase100_dump.txt"
echo Full dump written to %OUT%\dbase100_dump.txt
"%OUT%\dbase100_dump.exe" %1
