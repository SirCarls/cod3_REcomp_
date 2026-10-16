:; echo "setup.bat is for Windows. On Linux, run:  sh setup.sh" ; exit 1 #
@echo off
rem Starts the COD_3_WAW setup tool (Windows; also runs under Wine). It installs
rem anything missing (Python here; Rust, compilers and libraries when you press Build).
if not exist "%~dp0COD_3_WAW_Setup-tool\setup.bat" goto not_extracted
call "%~dp0COD_3_WAW_Setup-tool\setup.bat" %*
exit /b

:not_extracted
rem Started from inside a zip in File Explorer: Windows copied only this file.
echo The setup tool's files aren't next to this setup.bat ("%~dp0").
echo If you opened the downloaded zip in File Explorer, extract it first: right-click the
echo zip, Extract All, then run setup.bat in the extracted folder.
pause
exit /b 1
