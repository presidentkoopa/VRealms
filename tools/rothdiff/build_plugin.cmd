@echo off
REM ---------------------------------------------------------------------------
REM  Build rothdiff_plugin.c and install it as the oracle's rothdiff mod.
REM
REM  32-BIT, and MSVC rather than mingw. ROTH.C is a 32-bit process, so the
REM  plugin must be too; there is no mingw on this machine and the old mingw
REM  build is kept beside the output as plugin.dll.mingw. Visual Studio is 18,
REM  not 2022 -- several recorded recipes name the wrong path.
REM
REM  The previous plugin.dll is kept as plugin.dll.bak, because a plugin that
REM  fails to load looks exactly like a game that ignored the capture request.
REM
REM  Usage:  build_plugin.cmd
REM ---------------------------------------------------------------------------
setlocal

set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
set "SDK=E:\VRealms\tools\ROTH.C\sdk\include"
set "DEST=E:\DOOMWork\_oracle\mods\rothdiff"
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
cl /nologo /LD /W3 /O2 /I "%SDK%" rothdiff_plugin.c /Fe:plugin.dll
if errorlevel 1 ( echo BUILD FAILED & popd & exit /b 1 )
popd

REM  Only keep a backup that could actually be rolled back to. The first version
REM  of this script clobbered the installed DLL with a 25-byte text file, and the
REM  next run then dutifully backed THAT up over the last good one. A backup is
REM  worth having only while it is a DLL.
if exist "%DEST%\plugin.dll" (
	for %%F in ("%DEST%\plugin.dll") do if %%~zF GTR 1000 (
		copy /y "%DEST%\plugin.dll" "%DEST%\plugin.dll.bak" >nul
	) else (
		echo   note: installed plugin.dll is only %%~zF bytes, not backing it up
	)
)
copy /y "%HERE%plugin.dll" "%DEST%\plugin.dll" >nul
if errorlevel 1 ( echo ERROR: could not install to "%DEST%" & exit /b 1 )

REM  NO BARE ">" IN AN ECHO. "installed -> path" redirects the message INTO that
REM  path: the first run of this script overwrote the freshly installed DLL with
REM  a 25-byte text file, and a plugin that fails to load looks exactly like a
REM  game that ignored the capture request.
echo.
echo   built and installed: %DEST%\plugin.dll
exit /b 0
