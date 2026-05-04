@echo off
REM Build with Unigine SDK 2.19.0.3 (SVN)
echo === Building Terrain Editor Plugin (Unigine 2.19 SDK) ===
setlocal

REM -----------------------------------------------------------------------
REM  CONFIGURABLE: SDK and Project paths
REM -----------------------------------------------------------------------
set UNIGINE_SDK_DIR=E:\ALL_SVN\SNCFSNTrainSim\dependencies\cots\oksygen\cots\Unigine
set QT_VERSION=5.12.3
set QT_MSVC=msvc2017_64

REM Set paths
set PROJECT_ROOT=%~dp0
set BUILD_DIR=%PROJECT_ROOT%build_oksygen
set DEPLOY_DIR=%UNIGINE_SDK_DIR%\bin\plugins\Sogeclair\TerrainSurfaceTool
set PROJECT_DATA_DIR=%UNIGINE_SDK_DIR%\data
set SVN_PROJECT_DATA_DIR=E:\ALL_SVN\SNCFSNTrainSim\content\data
set TARGET_NAME=TerrainSurfaceTool
set BINARY_NAME=%TARGET_NAME%_editorplugin_double_x64
set QT_DIR=C:\Qt\Qt%QT_VERSION%\%QT_VERSION%\%QT_MSVC%

echo Project Root:    %PROJECT_ROOT%
echo Build Dir:       %BUILD_DIR%
echo Deploy Dir:      %DEPLOY_DIR%
echo Project Data:    %PROJECT_DATA_DIR%
echo Unigine SDK:     %UNIGINE_SDK_DIR%
echo Qt Dir:          %QT_DIR%
echo.

REM Validate SDK exists
if not exist "%UNIGINE_SDK_DIR%\include\Unigine.h" (
    echo ERROR: Unigine SDK not found at %UNIGINE_SDK_DIR%
    echo Please check the SDK path.
    exit /b 1
)

REM Validate Qt exists
if not exist "%QT_DIR%\bin\Qt5Core.dll" (
    echo ERROR: Qt %QT_VERSION% %QT_MSVC% not found at %QT_DIR%
    echo Please install Qt %QT_VERSION% [%QT_MSVC%] and update QT_VERSION/QT_MSVC above.
    exit /b 1
)

REM Copy plugin metadata to data folder
echo Copying plugin metadata...
if not exist "%PROJECT_DATA_DIR%\editor_plugins\" md "%PROJECT_DATA_DIR%\editor_plugins"
copy /y "%PROJECT_ROOT%\source\core\TerrainSurfaceToolEditorPlugin.json" "%PROJECT_DATA_DIR%\editor_plugins\" >nul
if %ERRORLEVEL% NEQ 0 (
    echo Warning: Failed to copy plugin metadata. Continuing anyway...
)

REM Copy content assets
echo Copying plugin content assets...
if not exist "%PROJECT_DATA_DIR%\editor_plugins\TerrainSurfaceTool\NUL" md "%PROJECT_DATA_DIR%\editor_plugins\TerrainSurfaceTool"
xcopy /s /y "%PROJECT_ROOT%\data\*" "%PROJECT_DATA_DIR%\editor_plugins\TerrainSurfaceTool\" >nul 2>&1

REM Copy basebrush files
echo Copying basebrush files...
if not exist "%PROJECT_DATA_DIR%\editor_plugins\TerrainSurfaceTool\Content\NUL" md "%PROJECT_DATA_DIR%\editor_plugins\TerrainSurfaceTool\Content"
copy /y "%PROJECT_ROOT%\Content\*.basebrush" "%PROJECT_DATA_DIR%\editor_plugins\TerrainSurfaceTool\Content\" >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo Warning: No basebrush files found to copy. Continuing anyway...
)

REM Setup Visual Studio environment
echo Setting up VS2022 environment...

set VCVARS64=
if exist "C:\PROGRA~2\MIB055~1\2022\BUILDT~1\VC\AUXILI~1\Build\VCVARS64.bat" set VCVARS64=C:\PROGRA~2\MIB055~1\2022\BUILDT~1\VC\AUXILI~1\Build\VCVARS64.bat
if not defined VCVARS64 if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set VCVARS64=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not defined VCVARS64 if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set VCVARS64=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not defined VCVARS64 if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set VCVARS64=C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
if not defined VCVARS64 if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set VCVARS64=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat

if not defined VCVARS64 (
    echo ERROR: vcvars64.bat not found. Please install Visual Studio 2022.
    exit /b 1
)

call "%VCVARS64%"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Failed to setup VS2022 environment.
    exit /b 1
)
echo VS2022 environment initialized!

REM Ensure Ninja is on PATH
set PATH=%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%

echo.
echo Step 1: Configuring with CMake...
if not exist "%BUILD_DIR%" md "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

if exist "CMakeCache.txt" del /f /q "CMakeCache.txt"
if exist "CMakeFiles" rmdir /s /q "CMakeFiles"

echo Using Ninja generator...
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release ^
    -DUNIGINE_SDK_DIR="%UNIGINE_SDK_DIR%" ^
    -DQT5_DIR="%QT_DIR%" ^
    -DDEPLOY_DIR="%DEPLOY_DIR%" ^
    -DCMAKE_PREFIX_PATH="%QT_DIR%"

if %ERRORLEVEL% NEQ 0 (
    echo CMake configuration failed!
    exit /b 1
)
echo CMake configuration successful!

echo.
echo Step 2: Building project...
ninja -v

if %ERRORLEVEL% NEQ 0 (
    echo Build failed!
    exit /b 1
)
echo Build completed successfully!

echo.
echo Step 3: Deploying plugin...
if not exist "%DEPLOY_DIR%" md "%DEPLOY_DIR%"

if exist "%DEPLOY_DIR%\%BINARY_NAME%.dll" (
    echo [OK] Plugin DLL deployed successfully
    echo      Location: %DEPLOY_DIR%\%BINARY_NAME%.dll
) else (
    echo [FAIL] Plugin DLL not found at deployment location
    echo        Expected: %DEPLOY_DIR%\%BINARY_NAME%.dll
)

copy /y "%PROJECT_ROOT%\source\core\TerrainSurfaceToolEditorPlugin.json" "%DEPLOY_DIR%\" >nul
echo [OK] Plugin metadata deployed

if not exist "%DEPLOY_DIR%\Content" md "%DEPLOY_DIR%\Content"
copy /y "%PROJECT_ROOT%\Content\*.basebrush" "%DEPLOY_DIR%\Content\" >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo [OK] Basebrush files deployed to SDK
) else (
    echo [INFO] No basebrush files to deploy to SDK
)

REM Copy basebrush files to SVN project data folder
if exist "%SVN_PROJECT_DATA_DIR%" (
    echo Copying basebrush to SVN project data...
    if not exist "%SVN_PROJECT_DATA_DIR%\editor_plugins\TerrainSurfaceTool\Content" md "%SVN_PROJECT_DATA_DIR%\editor_plugins\TerrainSurfaceTool\Content"
    copy /y "%PROJECT_ROOT%\Content\*.basebrush" "%SVN_PROJECT_DATA_DIR%\editor_plugins\TerrainSurfaceTool\Content\" >nul 2>&1
    if %ERRORLEVEL% EQU 0 (
        echo [OK] Basebrush files deployed to SVN project
    )
) else (
    echo [INFO] SVN project data folder not found: %SVN_PROJECT_DATA_DIR%
)

echo.
echo === Build Complete (Unigine SDK 2.19.0.3) ===
echo.
echo To use the plugin:
echo 1. Start Unigine Editor2 (2.19.0.3)
echo 2. Look for 'Terrain Tool' menu in the menu bar
echo 3. Select 'Pull Terrain To Surface'
echo 4. Select objects and click Apply
