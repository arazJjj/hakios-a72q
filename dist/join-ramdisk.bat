@echo off
cd /d "%~dp0"
if not exist ramdisk.img.part00 (echo Put ramdisk.img.part00 and ramdisk.img.part01 next to this file first. & pause & exit /b 1)
echo Joining... this takes a minute.
copy /b ramdisk.img.part00+ramdisk.img.part01 ramdisk.img >nul
if errorlevel 1 (echo Join failed. & pause & exit /b 1)
del ramdisk.img.part00 ramdisk.img.part01
echo Done: ramdisk.img is ready.
pause
