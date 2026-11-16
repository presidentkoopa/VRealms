@echo off
rem RUN_IMG3D.bat -- the item models: extract -> generate -> post (docs\IMG3D_PIPELINE.md). SETUP_IMG3D.bat first.
rem
rem   tools\img3d\RUN_IMG3D.bat                      the chapter 1 and 2 items (tools\img3d\items.json default_items)
rem   tools\img3d\RUN_IMG3D.bat 16 21                only these items
rem   tools\img3d\RUN_IMG3D.bat 16 --steps post      one step again (extract / generate / post, comma separated)
rem
rem Pictures and raw meshes: D:\VRealms_img3d\img3d_work\<item>\ (views.png shows the view chosen; put a file name in
rem choose.txt there to choose another). Models and manifest.json: D:\VRealms_img3d\items\. The engine finds
rem them through img3d_path.txt, written beside every doomxr.exe under build-dxr. Nothing is written into the repo.
setlocal
if "%IMG3D_ROOT%"=="" set IMG3D_ROOT=D:\VRealms_img3d
set PY=%IMG3D_ROOT%\img3d_env\Scripts\python.exe
if not exist "%PY%" (
	echo No venv at %IMG3D_ROOT%\img3d_env: run tools\img3d\SETUP_IMG3D.bat first.
	pause
	exit /b 1
)
set HY3DGEN_MODELS=%IMG3D_ROOT%\img3d_models
set HF_HOME=%IMG3D_ROOT%\img3d_models\hf
set PYTHONUTF8=1
"%PY%" "%~dp0run.py" %*
set RC=%errorlevel%
if not "%IMG3D_NOPAUSE%"=="1" pause
exit /b %RC%
