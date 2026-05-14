@echo off
:: build_installer_local.bat — invoked from PC1 via SSH after SCA/installer/
:: source has been staged at C:\SCA\installer\.
setlocal

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo VCVARS_FAIL & exit /b 1)

cd /d C:\SCA\installer
if errorlevel 1 (echo CD_FAIL & exit /b 1)

if not exist build mkdir build
cd build

cmake .. -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (echo CMAKE_CONFIGURE_FAIL & exit /b 1)

cmake --build . --config Release
if errorlevel 1 (echo CMAKE_BUILD_FAIL & exit /b 1)

echo BUILD_OK
dir Release\sca-svc.exe
