:; echo "setup.bat is for Windows. On Linux, run:  sh setup.sh" ; exit 1 #
@echo off
rem COD_3_WAW Setup tool - Windows bootstrap (also runs under Wine).
rem Makes sure a 64-bit Python 3 (3.9+, with Tkinter) is there, then starts the tool.
rem Arguments are passed through, e.g.  setup.bat doctor
rem The first line stops a Linux shell that is handed this file by mistake:
rem cmd reads it as a label and skips it.
rem Python is only installed with your approval, into this project folder
rem (work\tools\python), like everything else the tool installs.
rem No parenthesized blocks below: a ")" in a folder name would end them early.
setlocal
cd /d "%~dp0"
for %%I in ("%~dp0..") do set "PROJECT=%%~fI"
set "PYDIR=%PROJECT%\work\tools\python"

rem Started from inside a zip in File Explorer: Windows copied only this file
rem to a temporary folder, without the rest of the tool.
if not exist "%~dp0cod3_waw_setup.py" goto not_extracted

rem Wine creates this key in every prefix; Windows never has it.
set C3W_WINE=
reg query "HKCU\Software\Wine" >nul 2>&1 && set C3W_WINE=1
if defined C3W_WINE echo Running under Wine.

rem A usable Python: 3.9 or newer, 64-bit (a 32-bit one can't start every
rem build tool), with Tkinter (the window).
set "CHECK=import sys, tkinter; sys.exit(sys.version_info < (3, 9) or sys.maxsize < 2**32)"
set PY=
if exist "%PYDIR%\python.exe" "%PYDIR%\python.exe" -c "%CHECK%" >nul 2>&1 && set PY="%PYDIR%\python.exe"
if not defined PY py -3 -c "%CHECK%" >nul 2>&1 && set PY=py -3
if not defined PY python -c "%CHECK%" >nul 2>&1 && set PY=python
if defined PY goto run
if defined C3W_WINE goto wine_python
where winget >nul 2>&1
if errorlevel 1 goto no_winget

echo.
echo Python 3.12 is needed to run the setup tool. With your approval it is
echo downloaded with winget, Microsoft's package manager, and installed into
echo   "%PYDIR%"
echo in this project folder.
set "ANSWER="
set /p "ANSWER=Download and install Python 3.12 there? [y/N] "
if /i "%ANSWER%"=="y" goto install_python
if /i "%ANSWER%"=="yes" goto install_python
echo Nothing was downloaded.
pause
exit /b 1

:install_python
rem --source winget: not the Microsoft Store's catalog (blocked on some PCs).
rem --architecture x64: also on Windows on ARM, where the build needs the x64 one.
winget install -e --id Python.Python.3.12 --source winget --architecture x64 --scope user --location "%PYDIR%" --accept-package-agreements --accept-source-agreements
if exist "%PYDIR%\python.exe" "%PYDIR%\python.exe" -c "%CHECK%" >nul 2>&1 && set PY="%PYDIR%\python.exe"
if defined PY goto run
rem winget installs nothing when Python 3.12 is there already, and some
rem versions put it in its usual folder instead.
set "OTHER=%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
if not exist "%OTHER%" goto install_failed
"%OTHER%" -c "%CHECK%" >nul 2>&1 && set PY="%OTHER%"
if defined PY goto run
echo.
echo Python 3.12 is installed in "%LOCALAPPDATA%\Programs\Python\Python312"
echo but without Tkinter, which the setup tool's window needs. Add it: Settings,
echo Apps, Python 3.12, Modify, tick "tcl/tk and IDLE". Then run setup.bat again.
pause
exit /b 1

:install_failed
echo.
echo Python could not be installed automatically. Install the 64-bit Python from
echo https://www.python.org/downloads/windows/ (tick "tcl/tk and IDLE"), then run setup.bat again.
pause
exit /b 1

:no_winget
echo winget is not available. Install the 64-bit Python 3.12 from https://www.python.org/downloads/windows/
echo ^(tick "tcl/tk and IDLE"^), then run setup.bat again.
pause
exit /b 1

:not_extracted
echo The setup tool's files aren't next to this setup.bat ("%~dp0").
echo If you opened the downloaded zip in File Explorer, extract it first: right-click the
echo zip, Extract All, then run setup.bat in the extracted folder.
pause
exit /b 1

:wine_python
echo Python for Windows is not installed in this Wine prefix.
echo Download the Windows installer, 64-bit, from https://www.python.org/downloads/windows/
echo and run it with:  wine python-3.12.10-amd64.exe
echo then run setup.bat again.
echo Tip: on Linux, "sh setup.sh" builds the Windows version too, natively and faster than Wine.
pause
exit /b 1

:run
%PY% "%~dp0cod3_waw_setup.py" %*
if errorlevel 1 pause
