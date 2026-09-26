@echo off
rem ===========================================================================
rem auto-setup-windows-vr.cmd -- Windows VR build for DoomXR.
rem
rem Use this instead of auto-setup-windows.cmd, which does not work here.
rem
rem WHY NOT auto-setup-windows.cmd. It passes -DVCPKG_LIBSNDFILE=1, which pulls
rem libsndfile[mpeg] -> mpg123 -> yasm. The yasm port reads the LOCATION target
rem property, which CMake 4 removed, so it cannot configure at all. Visual
rem Studio 2026 ships CMake 4.3 and vcpkg fetches 4.4, so there is no
rem older-CMake escape -- the flag has to go. Without it zmusic needs only
rem zlib, and the engine itself needs nothing from vcpkg on a normal
rem (non-static-CRT) Windows build. The cost is a few uncommon music formats.
rem
rem WHY A SEPARATE BUILD FOLDER. build-dxr rather than build, so this never
rem fights an existing build directory for a locked DLL.
rem
rem Everything it needs that is not in the repo -- vcpkg, zmusic, the OpenVR
rem SDK -- it clones on first run into that folder. First build is slow;
rem afterwards it is incremental.
rem ===========================================================================

setlocal enableextensions

set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"
set "BUILD=%REPO%\build-dxr"

rem -- Locate Visual Studio, then the CMake it ships. CMake is generally not on
rem -- PATH even when Visual Studio is installed.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
	echo ERROR: vswhere not found -- is Visual Studio installed?
	exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
	echo ERROR: no Visual Studio installation found.
	exit /b 1
)

set "VSCMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
if not exist "%VSCMAKE%\cmake.exe" (
	echo ERROR: no cmake under "%VSCMAKE%".
	echo Install the "C++ CMake tools for Windows" component.
	exit /b 1
)
set "PATH=%VSCMAKE%;%PATH%"

echo === Visual Studio: %VSPATH%
cmake --version

if not exist "%BUILD%" mkdir "%BUILD%"
pushd "%BUILD%"

rem -- Forward slashes: CMake's find_path does not cope with backslashes here,
rem -- and silently leaves everything NOTFOUND when handed them.
set "VCPKGINST=%BUILD:\=/%/vcpkg/installed/x64-windows"
set "OVR=%BUILD:\=/%/openvr"

if not exist vcpkg  git clone https://github.com/microsoft/vcpkg
if not exist zmusic git clone https://github.com/zdoom/zmusic
if not exist openvr git clone --depth 1 https://github.com/ValveSoftware/openvr
if not exist zmusic\b2 mkdir zmusic\b2

rem -- OpenXR comes from vcpkg; the OpenVR SDK does not, because the engine
rem -- looks for Valve's own headers/openvr.h layout, which the port does not
rem -- provide.
rem -- --classic is required, not optional. This runs inside the repo, so vcpkg
rem -- finds the repo's own vcpkg.json, decides it is in manifest mode, and
rem -- refuses named packages outright. The repo manifest is for the engine's
rem -- dependencies; these two are ours.
if not exist vcpkg\vcpkg.exe call vcpkg\bootstrap-vcpkg.bat -disableMetrics
"%BUILD%\vcpkg\vcpkg.exe" install --classic openxr-loader:x64-windows openal-soft:x64-windows --vcpkg-root "%BUILD%\vcpkg"
if errorlevel 1 echo VCPKG INSTALL FAILED && popd && exit /b 1

if not exist zmusic\b2\CMakeCache.txt (
	echo === configure zmusic ===
	cmake -A x64 -S ./zmusic -B ./zmusic/b2 ^
		-DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake
	if errorlevel 1 echo ZMUSIC CONFIGURE FAILED && popd && exit /b 1

	echo === build zmusic ===
	cmake --build ./zmusic/b2 --config Release -- -maxcpucount -verbosity:minimal
	if errorlevel 1 echo ZMUSIC BUILD FAILED && popd && exit /b 1
)

rem -- ZDOOM_EXE_NAME is declared as a CACHE FILEPATH, so passing it without an
rem -- explicit :STRING makes CMake expand it to an absolute path and the link
rem -- fails with a doubled path. Do not drop the type.
echo === configure engine ===
cmake -A x64 -S .. -B . ^
	-DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake ^
	-DZMUSIC_INCLUDE_DIR=./zmusic/include ^
	-DZMUSIC_LIBRARIES=./zmusic/b2/source/Release/zmusic.lib ^
	-DZDOOM_EXE_NAME:STRING=doomxr ^
	-DENABLE_OPENXR=ON ^
	-DOPENXR_DIR=%VCPKGINST% ^
	-DENABLE_OPENVR=ON ^
	-DOPENVR_DIR=%OVR% ^
	-DOPENVR_SDK_PATH=%OVR% ^
	-DOPENVR_INCLUDE_DIRECTORY=%OVR%/headers ^
	-DOPENVR_LIBRARY=%OVR%/lib/win64/openvr_api.lib ^
	-DOPENVR_SHARED_LIBRARY=%OVR%/bin/win64/openvr_api.dll
if errorlevel 1 echo ENGINE CONFIGURE FAILED && popd && exit /b 1

echo === build engine ===
cmake --build . --config RelWithDebInfo -- -maxcpucount -verbosity:minimal
if errorlevel 1 echo ENGINE BUILD FAILED && popd && exit /b 1

rem -- Runtime DLLs are not copied by the build. Without zmusic.dll the game
rem -- will not start at all; without OpenAL32.dll it starts but floods the
rem -- console with AL_INVALID_ENUM, because Windows falls back to its own
rem -- ancient stub in System32.
set "OUT=%BUILD%\RelWithDebInfo"
copy /Y "%BUILD%\zmusic\b2\source\Release\zmusic.dll" "%OUT%\" >nul 2>&1
copy /Y "%BUILD%\vcpkg\installed\x64-windows\bin\OpenAL32.dll" "%OUT%\" >nul 2>&1
copy /Y "%BUILD%\vcpkg\installed\x64-windows\bin\fmt.dll" "%OUT%\" >nul 2>&1
copy /Y "%BUILD%\vcpkg\installed\x64-windows\bin\openxr_loader.dll" "%OUT%\" >nul 2>&1
rem -- openxr_loader.dll links against jsoncpp. Without it the loader fails to
rem -- load, OpenXR initialisation silently fails, and the engine falls back to
rem -- flat mode with no obvious error -- i.e. VR just never starts.
copy /Y "%BUILD%\vcpkg\installed\x64-windows\bin\jsoncpp.dll" "%OUT%\" >nul 2>&1
copy /Y "%BUILD%\openvr\bin\win64\openvr_api.dll" "%OUT%\" >nul 2>&1

popd

echo.
if exist "%OUT%\doomxr.exe" (
	echo BUILT: %OUT%\doomxr.exe
	echo.
	echo Put an IWAD ^(doom2.wad and friends^) in that folder to play.
) else (
	echo Build reported success but no doomxr.exe was produced.
	exit /b 1
)
