@echo off
rem SETUP_IMG3D.bat -- run ONCE: the image-to-3D pipeline's venv, Hunyuan3D-2, its weights, the upscaler, the
rem extractor and a self-test (docs\IMG3D_PIPELINE.md). Everything goes under D:\VRealms_img3d (set IMG3D_ROOT to
rem change it), never into the repository. About 25 GB and an hour or more the first time; run it again to resume.
rem
rem   tools\img3d\SETUP_IMG3D.bat [--yes-licence] [--skip-selftest]
setlocal
set HERE=%~dp0
echo SETUP_IMG3D: each step is printed below as [n/11]; if it stops, the reason is the last thing printed.
where py >nul 2>&1
if not errorlevel 1 (
	py -3 "%HERE%setup_img3d.py" %*
) else (
	python "%HERE%setup_img3d.py" %*
)
if errorlevel 1 (
	echo.
	echo SETUP_IMG3D did not finish: the reason is printed above.
	pause
	exit /b 1
)
pause
exit /b 0
