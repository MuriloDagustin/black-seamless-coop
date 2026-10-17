@echo off
setlocal
rem Black Seamless Co-op: puts the co-op mod onto the Black XBE Recomp port, at the versions it was made with.
rem   setup.bat [folder] [game.iso]
rem folder: where the port goes (default: black-recomp beside this file)
rem game.iso: your own disc image of Black (USA), extracted into the port's assets folder
title Black Seamless Co-op - Setup
set "MOD=%~dp0"
set "DIR=%~1"
if "%DIR%"=="" set "DIR=%MOD%black-recomp"
set "ISO=%~2"
set "PATCH=%MOD%patches\upstream.patch"
set "PORT_URL=https://github.com/TGP482/BLACK-XBERecomp.git"
set "SDK_URL=https://github.com/TGP482/XboxXBE-Recomp.git"
set "PORT_COMMIT=39ad6973d3eed447acda81f99722ac27e6ee0b71"
set "SDK_COMMIT=40e9c35451305f09ca9d86f677e4b9c2c37ff26b"

where git >nul 2>nul || (echo   git was not found: https://git-scm.com/download/win& goto :fail)

echo [1/4] The port, at %PORT_COMMIT:~0,7%
if not exist "%DIR%\.git" (
    git clone --quiet "%PORT_URL%" "%DIR%" || goto :fail
)
git -C "%DIR%" checkout --quiet %PORT_COMMIT% || goto :fail

echo [2/4] The recompiler SDK, at %SDK_COMMIT:~0,7%
rem the port's CMakeLists.txt takes an SDK in its sdk folder before any other
if not exist "%DIR%\sdk\.git" (
    git clone --quiet "%SDK_URL%" "%DIR%\sdk" || goto :fail
)
git -C "%DIR%\sdk" checkout --quiet %SDK_COMMIT% || goto :fail

echo [3/4] The co-op
git -C "%DIR%" apply --reverse --check --ignore-whitespace "%PATCH%" >nul 2>nul
if errorlevel 1 (
    git -C "%DIR%" apply --ignore-whitespace "%PATCH%" || goto :fail
) else (
    echo       the patch is in already
)
copy /y "%MOD%src\game\coop*.cpp" "%DIR%\src\game\" >nul || goto :fail
copy /y "%MOD%src\game\coop*.h" "%DIR%\src\game\" >nul || goto :fail

echo [4/4] The game
if exist "%DIR%\assets\default.xbe" (
    echo       assets\default.xbe is there
) else if not "%ISO%"=="" (
    where python >nul 2>nul || (echo   python was not found, it extracts the disc image: https://www.python.org& goto :fail)
    python -I "%MOD%tools\xiso_extract.py" "%ISO%" "%DIR%\assets" || goto :fail
) else (
    echo       Put your own copy of Black in %DIR%\assets:
    echo       default.xbe with GlobData.bin and the chars, data, export, language, levels, sound and videos
    echo       folders beside it, or run this again with your disc image: setup.bat "%DIR%" "Black.iso"
)

echo.
echo   Done. Build it with %DIR%\build.bat
echo   (Visual Studio with C++, Clang and CMake; the first build takes a while)
echo.
exit /b 0

:fail
echo.
echo   SETUP FAILED. Read the last message above.
echo.
exit /b 1
