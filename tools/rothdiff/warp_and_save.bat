@echo off
REM ---------------------------------------------------------------------------
REM  Put a quicksave in the map the comparison rig needs to capture.
REM
REM  ROTH.C has no "start in map X" switch, so the way in is the developer map
REM  warp, which --devmode holds unlocked. The rig skips the intro by pressing
REM  F10 (quickload), so whatever map the quicksave sits in is the map that can
REM  be captured -- which is why this exists.
REM
REM  NOTE: this uses the ONE quicksave slot. Making a quicksave in a new map
REM  replaces the one that is there, so the previous map stops being capturable
REM  until a quicksave is made in it again.
REM ---------------------------------------------------------------------------
echo.
echo   1.  Press  W       the developer map warp
echo   2.  Choose LRINTH1 (or whichever map you were asked for)
echo   3.  Press  F9      quicksave  -- this is the bit that matters
echo   4.  Close the window
echo.
echo   F9 saves, F10 loads. There is no menu button; those two keys are it.
echo.
pause
cd /d E:\DOOMWork\_oracle
rothc.exe --windowed --devmode --game-dir E:\DOOMWork\_oracle --c-root E:\DOOMWork\_croot
