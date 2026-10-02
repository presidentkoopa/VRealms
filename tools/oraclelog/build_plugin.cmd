@echo off
REM ---------------------------------------------------------------------------
REM  Build oraclelog.c and install it as the oracle's oraclelog mod.
REM
REM  32-bit MSVC, same as tools/rothdiff/build_plugin.cmd -- ROTH.C is a 32-bit
REM  process and there is no mingw on this machine. Visual Studio is 18.
REM
REM  NO BARE ">" IN AN ECHO below: "installed -> path" redirects the message INTO
REM  that path and leaves a 25-byte text file where the DLL should be.
REM ---------------------------------------------------------------------------
setlocal

set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
set "SDK=E:\VRealms\tools\ROTH.C\sdk\include"
set "DEST=E:\DOOMWork\_oracle\mods\oraclelog"
set "HERE=%~dp0"

if not exist "%VS%\VC\Auxiliary\Build\vcvars32.bat" (
	echo ERROR: no vcvars32.bat under "%VS%".
	exit /b 1
)
if not exist "%SDK%\roth_sdk.h" (
	echo ERROR: no roth_sdk.h under "%SDK%".
	exit /b 1
)

call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 ( echo ERROR: vcvars32 failed & exit /b 1 )

pushd "%HERE%"
cl /nologo /LD /W3 /O2 /I "%SDK%" oraclelog.c /Fe:plugin.dll
if errorlevel 1 ( echo BUILD FAILED & popd & exit /b 1 )
popd

if exist "%DEST%\plugin.dll" (
	for %%F in ("%DEST%\plugin.dll") do if %%~zF GTR 1000 (
		copy /y "%DEST%\plugin.dll" "%DEST%\plugin.dll.bak" >nul
	) else (
		echo   note: installed plugin.dll is only %%~zF bytes, not backing it up
	)
)
copy /y "%HERE%plugin.dll" "%DEST%\plugin.dll" >nul
if errorlevel 1 ( echo ERROR: could not install to "%DEST%" & exit /b 1 )

echo.
echo   built and installed: %DEST%\plugin.dll
exit /b 0
