@echo off
rem Build and run the standalone ROTH reader checks.
rem
rem Deliberately builds into a scratch folder: leaving object files beside the
rem sources makes the engine's own build fail with a PDB conflict.
setlocal
set HERE=%~dp0
set OUT=%HERE%..\..\build-dxr\roth_selftest
if not exist "%OUT%" mkdir "%OUT%"

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

cl /nologo /std:c++17 /EHsc /D_CRT_SECURE_NO_WARNINGS ^
   /Fo:"%OUT%\\" /Fd:"%OUT%\selftest.pdb" /Fe:"%OUT%\roth_selftest.exe" ^
   "%HERE%roth_selftest.cpp" "%HERE%roth_raw.cpp" "%HERE%roth_commands.cpp" "%HERE%roth_das.cpp" ^
   "%HERE%roth_palette.cpp" "%HERE%roth_install.cpp"
if errorlevel 1 exit /b 1

if "%~1"=="" (
	echo.
	echo Usage: build_selftest.cmd "path\to\Realms of the Haunting\ROTH"
	exit /b 0
)
"%OUT%\roth_selftest.exe" %1
