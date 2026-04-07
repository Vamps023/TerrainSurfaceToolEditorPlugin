@echo off
REM Build with Ninja generator
echo === Building Vamps Terrain Editor Plugin (Ninja) ===
setlocal

REM Set paths
set PROJECT_ROOT=%~dp0
set BUILD_DIR=%PROJECT_ROOT%build
set DEPLOY_DIR=C:\Users\snare.ext\Documents\UNIGINE Projects\unigine_project_3\bin\plugins\Vamps\TerrainSurfaceTool
set PROJECT_DATA_DIR=C:\Users\snare.ext\Documents\UNIGINE Projects\unigine_project_3\data
set TARGET_NAME=TerrainSurfaceTool
set BINARY_NAME=%TARGET_NAME%_editorplugin_double_x64

echo Project Root: %PROJECT_ROOT%
echo Build Dir: %BUILD_DIR%
echo Deploy Dir: %DEPLOY_DIR%
echo Project Data Dir: %PROJECT_DATA_DIR%
echo Target Name: %TARGET_NAME%
echo Binary Name: %BINARY_NAME%
echo.

REM Create build directory
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%DEPLOY_DIR%" mkdir "%DEPLOY_DIR%"
if not exist "%PROJECT_DATA_DIR%" mkdir "%PROJECT_DATA_DIR%"

REM Copy plugin metadata
echo Copying plugin metadata...
copy "%PROJECT_ROOT%source\TerrainSurfaceToolEditorPlugin.json" "%DEPLOY_DIR%\" >nul 2>&1
echo Copying plugin content assets...
copy "%PROJECT_ROOT%Content\*.basebrush" "%PROJECT_DATA_DIR%\" >nul 2>&1

REM Setup VS2022 Developer Command Prompt
echo Setting up VS2022 Developer Command Prompt...
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul 2>&1

echo VS2022 Developer Command Prompt initialized!

REM Configure with CMake
echo Step 1: Configuring with CMake...
cd /d "%BUILD_DIR%"

REM Clear any existing cache completely
if exist "CMakeCache.txt" del /f /q "CMakeCache.txt"
if exist "CMakeFiles" rmdir /s /q "CMakeFiles"

REM Use Ninja generator
echo Using Ninja generator...
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
if %ERRORLEVEL% NEQ 0 (
    echo CMake configuration failed!
    exit /b 1
)

echo CMake configuration successful!

REM Build the project
echo.
echo Step 2: Building project...
cmake --build . --config Release
if %ERRORLEVEL% NEQ 0 (
    echo Build failed!
    echo.
    echo Trying direct ninja build...
    ninja
    if %ERRORLEVEL% NEQ 0 (
        echo Direct ninja build failed too!
        exit /b 1
    )
)

echo Build completed successfully!

REM Deploy the plugin
echo.
echo Step 3: Deploying plugin...
if exist "%DEPLOY_DIR%\%BINARY_NAME%.dll" (
    echo ✓ Plugin DLL deployed successfully
    echo   Location: %DEPLOY_DIR%\%BINARY_NAME%.dll
) else (
    echo ✗ Plugin DLL not found at deployment location
    echo Searching for DLL...
    dir /s "%BUILD_DIR%\*.dll"
    echo.
    echo Manual deployment may be required.
)

if exist "%DEPLOY_DIR%\TerrainSurfaceToolEditorPlugin.json" (
    echo ✓ Plugin metadata deployed
) else (
    echo ✗ Plugin metadata not found
)

echo.
echo === Build Complete ===
if exist "%PROJECT_DATA_DIR%\terrain_brush_r32f_overwrite.basebrush" (
    echo Brush assets deployed to %PROJECT_DATA_DIR%
) else (
    echo Brush assets not found in %PROJECT_DATA_DIR%
)
echo.
echo Plugin Structure (GantryLabelTool Pattern):
echo %DEPLOY_DIR%
echo ├── %BINARY_NAME%.dll
echo └── TerrainSurfaceToolEditorPlugin.json
echo.
echo Project Data Assets:
echo %PROJECT_DATA_DIR%
echo terrain_brush_r32f_overwrite.basebrush
echo.
echo To use the plugin:
echo 1. Start Unigine Editor2
echo 2. Look for 'Terrain Tool' menu in the menu bar
echo 3. Select 'Pull Terrain To Surface'
echo 4. Select objects and click Apply
echo.
exit /b 0
