@echo off
setlocal EnableExtensions
title HeavenInternal - GitHub guncellemesi olustur

cd /d "%~dp0"

where git >nul 2>&1
if errorlevel 1 (
    echo [HATA] Git bulunamadi. Git for Windows kurun.
    pause
    exit /b 1
)

if not exist "HeavenInternal.dll" (
    echo [HATA] Koku dizinde HeavenInternal.dll bulunamadi.
    echo Once Release derlemesini koku dizine kopyalayin.
    pause
    exit /b 1
)

set "GH_REPO=len1th/heaven-internal"
set /p "GH_REPO=GitHub repository (owner/repository) [%GH_REPO%]: "
if not defined GH_REPO set "GH_REPO=len1th/heaven-internal"
set "GH_REPO=%GH_REPO: =%"

set "GH_BRANCH=main"
set /p "GH_BRANCH=Branch [%GH_BRANCH%]: "
if not defined GH_BRANCH set "GH_BRANCH=main"
set "GH_BRANCH=%GH_BRANCH: =%"

set "VERSION=1.0.1"
set /p "VERSION=Yeni surum [%VERSION%]: "
if not defined VERSION set "VERSION=1.0.1"
set "VERSION=%VERSION: =%"

echo.
echo [BILGI] DLL SHA-256 hesaplaniyor...
for /f "usebackq delims=" %%H in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "(Get-FileHash -LiteralPath 'HeavenInternal.dll' -Algorithm SHA256).Hash.ToLower()"`) do set "HASH=%%H"
if not defined HASH (
    echo [HATA] SHA-256 hesaplanamadi.
    pause
    exit /b 1
)

>update.json echo {
>>update.json echo   "version": "%VERSION%",
>>update.json echo   "url": "/%GH_REPO%/%GH_BRANCH%/HeavenInternal.dll",
>>update.json echo   "sha256": "%HASH%"
>>update.json echo }

echo.
echo [BILGI] Olusturulan manifest:
type update.json
echo.

git rev-parse --is-inside-work-tree >nul 2>&1
if errorlevel 1 (
    echo [HATA] Bu klasor bir Git repository degil.
    pause
    exit /b 1
)

git remote get-url origin >nul 2>&1
if errorlevel 1 (
    echo [HATA] origin remote bulunamadi.
    echo GitHub remote ekleyin: git remote add origin https://github.com/%GH_REPO%.git
    pause
    exit /b 1
)

rem DLL .gitignore icinde olsa bile release dosyasi zorla eklenir.
git add -f -- "HeavenInternal.dll"
git add -- "update.json"
git commit -m "Release %VERSION%"
if errorlevel 1 (
    echo [HATA] Commit olusturulamadi.
    pause
    exit /b 1
)

git push origin "%GH_BRANCH%"
if errorlevel 1 (
    echo [HATA] GitHub'a push basarisiz. Git kimlik bilgilerinizi kontrol edin.
    pause
    exit /b 1
)

echo.
echo [BASARILI] Guncelleme GitHub'a gonderildi.
echo Manifest: https://raw.githubusercontent.com/%GH_REPO%/%GH_BRANCH%/update.json
echo.
pause
