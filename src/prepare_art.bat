@echo off
rem The loader embeds the three games' cover images. They are not in this repository; when they are
rem missing the placeholders under art\placeholder are copied into art\ so the resource script builds.
for %%f in (tile_bfme2.jpg tile_rotwk.jpg tile_aotr.jpg) do (
  if not exist "%~dp0art\%%f" copy /y "%~dp0art\placeholder\%%f" "%~dp0art\%%f" >nul
)
