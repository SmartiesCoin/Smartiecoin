@echo off
setlocal enabledelayedexpansion
title Mine Smartiecoin (SMT)
set THREADS=8
set ADDR=

rem ==== to change anything, edit ONLY these two lines above ====
rem THREADS: how many CPU cores/threads to use (8 works well on a work PC)
rem ADDR:    your SMT address (get it from your wallet: Receive tab)

set SM=%~dp0SMT Miner
set DD=%APPDATA%\SmartiecoinCore
if not exist "%DD%\blocks" goto nodata
set CONF=%DD%\smartiecoin.conf

rem ==== make sure the wallet has local RPC enabled ====
findstr /b /c:"server=1" "%CONF%" >nul 2>&1 || (
  echo.>>"%CONF%"
  echo server=1>>"%CONF%"
  echo rpcallowip=127.0.0.1>>"%CONF%"
)
findstr /b /c:"rpcuser=" "%CONF%" >nul 2>&1 || (
  echo.>>"%CONF%"
  echo rpcuser=smtminer>>"%CONF%"
  echo rpcpassword=!RANDOM!!RANDOM!!RANDOM!!RANDOM!>>"%CONF%"
)

if "%ADDR%"=="" set /p ADDR=Paste your Smartiecoin address (Receive address from your wallet):
if "%ADDR%"=="" ( echo [!] Address is missing. & pause & exit /b 1 )

echo ============================================
echo   MINING SMARTIECOIN   Address: %ADDR%
echo ============================================
echo.
echo [1/3] Checking wallet...
"%SM%\smartiecoin-cli.exe" -datadir="%DD%" getblockcount >nul 2>&1
if not errorlevel 1 goto wallet_ok

echo    The wallet is not responding to RPC yet.
echo    Restarting the wallet (if it is open, it will be closed and reopened)...
taskkill /IM smartiecoin-qt.exe >nul 2>&1
timeout /t 8 /nobreak >nul
if exist "%~dp0smartiecoin-qt.exe" (
  start "" "%~dp0smartiecoin-qt.exe" -datadir="%DD%"
) else (
  echo.
  echo    [!] Please open your Smartiecoin wallet manually now. Waiting...
)
set /a t=0
:wait
timeout /t 5 /nobreak >nul
"%SM%\smartiecoin-cli.exe" -datadir="%DD%" getblockcount >nul 2>&1
if not errorlevel 1 goto wallet_ok
set /a t+=1
if %t% lss 30 goto wait
echo.
echo    [!] The wallet is not responding to RPC.
echo        Close the wallet manually, reopen it, then run this .bat file again.
pause
exit /b 1

:nodata
echo [!] Smartiecoin wallet data was not found on this computer.
echo     Open your Smartiecoin wallet at least once, then run this .bat file again.
pause
exit /b 1

:wallet_ok
echo    Wallet OK.
echo.
echo [2/3] Mining with %THREADS% threads - Press Ctrl+C or close this window to stop
echo.
"%SM%\smt-miner.exe" %ADDR% --threads %THREADS% --conf "%CONF%" --rpc http://127.0.0.1:8282
echo.
echo [3/3] The miner has stopped.
pause