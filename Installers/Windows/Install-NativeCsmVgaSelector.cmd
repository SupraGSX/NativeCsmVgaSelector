@echo off
setlocal
set "NCV_INSTALLER_DIR=%~dp0"
set "NCV_INSTALLER_PS1=%~dp0Installer.ps1"

if not exist "%NCV_INSTALLER_PS1%" (
  echo ERROR: Installer.ps1 was not found beside this launcher.
  exit /b 1
)

where powershell.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: Windows PowerShell is unavailable.
  exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop'; $d=[IO.Path]::GetFullPath($env:NCV_INSTALLER_DIR); if($d.EndsWith('\')){$d+='.'}; $s=[IO.Path]::GetFullPath($env:NCV_INSTALLER_PS1); $p=New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent()); if($p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){& $s -InstallerDirectory $d -WaitOnExit; exit $LASTEXITCODE}; $q=[char]34; $a='-NoProfile -ExecutionPolicy Bypass -File '+$q+$s+$q+' -InstallerDirectory '+$q+$d+$q+' -WaitOnExit'; $c=Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -Verb RunAs -ArgumentList $a -Wait -PassThru; exit $c.ExitCode"

set "NCV_EXIT_CODE=%errorlevel%"
if not "%NCV_EXIT_CODE%"=="0" echo ERROR: The installer did not complete.
exit /b %NCV_EXIT_CODE%
