@echo off
REM ==============================================================================
REM LAIS (Local AI Studio) v3.6.1 - Modular Windows x64 Builder & Packager
REM ------------------------------------------------------------------------------
REM Creates all required directories if missing, preserves existing JSON files in
REM %USERPROFILE%\lais\configs\ and %USERPROFILE%\lais\data\, compiles src\*.cpp
REM into build\lais.exe, and creates build\lais.zip.
REM ==============================================================================

setlocal EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
cd /d "%SCRIPT_DIR%"

if "%LAIS_DIR%"=="" set "LAIS_DIR=%USERPROFILE%\lais"
set "CONFIGS_DIR=%LAIS_DIR%\configs"
set "DATA_DIR=%LAIS_DIR%\data"
set "CHATS_DIR=%LAIS_DIR%\chats"
set "MODELS_DIR=%LAIS_DIR%\models"
set "OUTPUTS_DIR=%LAIS_DIR%\outputs"
set "LOGS_DIR=%LAIS_DIR%\logs"
set "ENGINES_DIR=%LAIS_DIR%\engines"
set "BIN_DIR=%LAIS_DIR%\bin"
set "CACHE_DIR=%LAIS_DIR%\cache"
set "BUILD_OUT_DIR=%SCRIPT_DIR%\build"

set "FORCE_REBUILD=0"
set "LAIS_ONLY=0"

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="--rebuild" set "FORCE_REBUILD=1"
if /I "%~1"=="--force" set "FORCE_REBUILD=1"
if /I "%~1"=="--lais-only" set "LAIS_ONLY=1"
shift
goto parse_args
:args_done

if not exist "%LAIS_DIR%" mkdir "%LAIS_DIR%"
if not exist "%CONFIGS_DIR%" mkdir "%CONFIGS_DIR%"
if not exist "%DATA_DIR%" mkdir "%DATA_DIR%"
if not exist "%CHATS_DIR%" mkdir "%CHATS_DIR%"
if not exist "%MODELS_DIR%" mkdir "%MODELS_DIR%"
if not exist "%OUTPUTS_DIR%" mkdir "%OUTPUTS_DIR%"
if not exist "%LOGS_DIR%" mkdir "%LOGS_DIR%"
if not exist "%ENGINES_DIR%" mkdir "%ENGINES_DIR%"
if not exist "%BIN_DIR%\configs" mkdir "%BIN_DIR%\configs"
if not exist "%BIN_DIR%\data" mkdir "%BIN_DIR%\data"
if not exist "%CACHE_DIR%\tmp" mkdir "%CACHE_DIR%\tmp"
if not exist "%BUILD_OUT_DIR%\obj" mkdir "%BUILD_OUT_DIR%\obj"

REM Upgrade older model/engine catalogs once, preserving backups and user settings.
powershell -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%SCRIPT_DIR%\tools\update_configs.ps1" -Source "%SCRIPT_DIR%\configs" -Destination "%CONFIGS_DIR%"
if errorlevel 1 exit /b 1
REM Seed the remaining configs only when missing.
for %%c in (settings.json sources.json models.json en-us.lang ko-kr.lang) do (
    if not exist "%CONFIGS_DIR%\%%c" (
        if exist "%SCRIPT_DIR%\configs\%%c" copy /Y "%SCRIPT_DIR%\configs\%%c" "%CONFIGS_DIR%\%%c" >NUL
    )
    if exist "%CONFIGS_DIR%\%%c" (
        copy /Y "%CONFIGS_DIR%\%%c" "%BIN_DIR%\configs\%%c" >NUL
    ) else if exist "%SCRIPT_DIR%\configs\%%c" (
        copy /Y "%SCRIPT_DIR%\configs\%%c" "%BIN_DIR%\configs\%%c" >NUL
    )
)

if not exist "%DATA_DIR%\user.json" (
    if exist "%SCRIPT_DIR%\data\user.json" copy /Y "%SCRIPT_DIR%\data\user.json" "%DATA_DIR%\user.json" >NUL
)
if exist "%DATA_DIR%\user.json" (
    copy /Y "%DATA_DIR%\user.json" "%BIN_DIR%\data\user.json" >NUL
) else if exist "%SCRIPT_DIR%\data\user.json" (
    copy /Y "%SCRIPT_DIR%\data\user.json" "%BIN_DIR%\data\user.json" >NUL
)

set "SOURCES_JSON=%CONFIGS_DIR%\sources.json"
if not exist "%SOURCES_JSON%" set "SOURCES_JSON=%SCRIPT_DIR%\configs\sources.json"

for /f "usebackq tokens=*" %%i in (`powershell -NoLogo -NoProfile -NonInteractive -Command "(Get-Content '%SOURCES_JSON%' | ConvertFrom-Json).llama_cpp_repo"`) do set "LLAMA_REPO=%%i"
for /f "usebackq tokens=*" %%i in (`powershell -NoLogo -NoProfile -NonInteractive -Command "(Get-Content '%SOURCES_JSON%' | ConvertFrom-Json).stable_diffusion_cpp_repo"`) do set "SD_REPO=%%i"
for /f "usebackq tokens=*" %%i in (`powershell -NoLogo -NoProfile -NonInteractive -Command "(Get-Content '%SOURCES_JSON%' | ConvertFrom-Json).stable_diffusion_cpp_tag"`) do set "SD_TAG=%%i"
for /f "usebackq tokens=*" %%i in (`powershell -NoLogo -NoProfile -NonInteractive -Command "(Get-Content '%SOURCES_JSON%' | ConvertFrom-Json).acestep_cpp_repo"`) do set "ACE_REPO=%%i"

echo ==============================================================================
echo  LAIS (Local AI Studio) v3.6.1 - Modular Windows x64 Builder ^& Packager
echo ==============================================================================

where cl.exe >NUL 2>&1
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
            set "VS_PATH=%%i"
        )
        if exist "!VS_PATH!\VC\Auxiliary\Build\vcvars64.bat" (
            echo [1/5] Initializing Visual Studio x64 Developer Environment...
            call "!VS_PATH!\VC\Auxiliary\Build\vcvars64.bat" >NUL
        )
    )
)

set "HAS_CUDA=0"
set "CUDA_CMAKE_FLAGS="
where nvcc.exe >NUL 2>&1
if not errorlevel 1 (
    set "HAS_CUDA=1"
    for /f "usebackq tokens=*" %%g in (`nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2^>NUL`) do (
        set "RAW_ARCH=%%g"
        set "GPU_ARCH=!RAW_ARCH:.=!"
    )
    if "!GPU_ARCH!"=="" set "GPU_ARCH=native"
    set "CUDA_CMAKE_FLAGS=-DCMAKE_CUDA_ARCHITECTURES=!GPU_ARCH! -DCMAKE_CUDA_FLAGS=-allow-unsupported-compiler"
    echo [2/5] Detected NVIDIA CUDA Toolkit (GPU Arch: !GPU_ARCH!)
) else (
    echo [2/5] CUDA nvcc.exe not detected in PATH; building with CPU fallback (or existing engines).
)

if "%LAIS_ONLY%"=="0" (
    echo [3/5] Building C++ AI Inference Engines in "%ENGINES_DIR%"...

    set "LLAMA_DIR=%ENGINES_DIR%\llama.cpp"
    set "BUILD_LLAMA=%FORCE_REBUILD%"
    if not exist "!LLAMA_DIR!\build\bin\Release\llama-server.exe" if not exist "!LLAMA_DIR!\build\bin\llama-server.exe" (
        set "BUILD_LLAMA=1"
    )
    if "!BUILD_LLAMA!"=="1" (
        if not exist "!LLAMA_DIR!\.git" (
            git clone "!LLAMA_REPO!" "!LLAMA_DIR!"
        ) else (
            git -C "!LLAMA_DIR!" fetch origin master
            git -C "!LLAMA_DIR!" reset --hard origin/master
        )
        if "%HAS_CUDA%"=="1" (set "GGML_CUDA_OPT=-DGGML_CUDA=ON") else (set "GGML_CUDA_OPT=-DGGML_CUDA=OFF")
        cmake -S "!LLAMA_DIR!" -B "!LLAMA_DIR!\build" -DCMAKE_BUILD_TYPE=Release !GGML_CUDA_OPT! -DBUILD_SHARED_LIBS=OFF -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=ON !CUDA_CMAKE_FLAGS!
        cmake --build "!LLAMA_DIR!\build" --config Release --target llama-server llama-cli -j 8
    )

    set "SD_DIR=%ENGINES_DIR%\stable-diffusion.cpp"
    set "BUILD_SD=%FORCE_REBUILD%"
    if not exist "!SD_DIR!\build\bin\Release\sd-cli.exe" if not exist "!SD_DIR!\build\bin\sd-cli.exe" (
        set "BUILD_SD=1"
    )
    if "!BUILD_SD!"=="1" (
        if not exist "!SD_DIR!\.git" (
            git clone --depth 1 --branch !SD_TAG! --recurse-submodules "!SD_REPO!" "!SD_DIR!"
        ) else (
            git -C "!SD_DIR!" fetch --depth 1 origin tag !SD_TAG!
            git -C "!SD_DIR!" checkout -f !SD_TAG!
            git -C "!SD_DIR!" submodule update --init --recursive --force
        )
        if "%HAS_CUDA%"=="1" (set "SD_CUDA_OPT=-DSD_CUDA=ON") else (set "SD_CUDA_OPT=-DSD_CUDA=OFF")
        cmake -S "!SD_DIR!" -B "!SD_DIR!\build" -DCMAKE_BUILD_TYPE=Release !SD_CUDA_OPT! -DSD_BUILD_SHARED_LIBS=OFF !CUDA_CMAKE_FLAGS!
        cmake --build "!SD_DIR!\build" --config Release -j 8
    )

    set "ACE_DIR=%ENGINES_DIR%\acestep.cpp"
    set "BUILD_ACE=%FORCE_REBUILD%"
    if not exist "!ACE_DIR!\build\Release\ace-synth.exe" if not exist "!ACE_DIR!\build\ace-synth.exe" (
        set "BUILD_ACE=1"
    )
    if "!BUILD_ACE!"=="1" (
        if not exist "!ACE_DIR!\.git" (
            git clone --depth 1 --recurse-submodules "!ACE_REPO!" "!ACE_DIR!"
        ) else (
            if "!FORCE_REBUILD!"=="1" (
                git -C "!ACE_DIR!" fetch --depth 1 origin master
                if errorlevel 1 exit /b 1
                git -C "!ACE_DIR!" checkout --detach FETCH_HEAD
                if errorlevel 1 exit /b 1
            )
            git -C "!ACE_DIR!" submodule update --init --recursive
        )
        if "%HAS_CUDA%"=="1" (set "ACE_CUDA_OPT=-DGGML_CUDA=ON") else (set "ACE_CUDA_OPT=-DGGML_CUDA=OFF")
        cmake -S "!ACE_DIR!" -B "!ACE_DIR!\build" -DCMAKE_BUILD_TYPE=Release !ACE_CUDA_OPT! -DBUILD_SHARED_LIBS=OFF !CUDA_CMAKE_FLAGS!
        cmake --build "!ACE_DIR!\build" --config Release -j 8
    )

    for %%f in (
        "!LLAMA_DIR!\build\bin\Release\llama-server.exe"
        "!LLAMA_DIR!\build\bin\llama-server.exe"
        "!LLAMA_DIR!\build\bin\Release\llama-cli.exe"
        "!LLAMA_DIR!\build\bin\llama-cli.exe"
        "!SD_DIR!\build\bin\Release\sd-cli.exe"
        "!SD_DIR!\build\bin\sd-cli.exe"
        "!ACE_DIR!\build\Release\ace-lm.exe"
        "!ACE_DIR!\build\ace-lm.exe"
        "!ACE_DIR!\build\Release\ace-synth.exe"
        "!ACE_DIR!\build\ace-synth.exe"
    ) do (
        if exist "%%~f" copy /Y "%%~f" "%BIN_DIR%\" >NUL
    )
)

echo [4/5] Compiling modular C++20 sources (src\*.cpp) in parallel into build\lais.exe...
set "RAW_BIN=%BUILD_OUT_DIR%\lais.raw.exe"
set "OUT_BIN=%BUILD_OUT_DIR%\lais.exe"

where cl.exe >NUL 2>&1
if not errorlevel 1 (
    cl.exe /nologo /std:c++20 /O2 /DNDEBUG /EHsc /utf-8 /MP /I"%SCRIPT_DIR%\src" ^
        "%SCRIPT_DIR%\src\common.cpp" ^
        "%SCRIPT_DIR%\src\telemetry.cpp" ^
        "%SCRIPT_DIR%\src\crypto_pake.cpp" ^
        "%SCRIPT_DIR%\src\coordinator.cpp" ^
        "%SCRIPT_DIR%\src\http_server.cpp" ^
        "%SCRIPT_DIR%\src\engine_manager.cpp" ^
        "%SCRIPT_DIR%\src\main.cpp" ^
        "%SCRIPT_DIR%\src\line_editor.cpp" ^
        /Fo"%BUILD_OUT_DIR%\obj\\" /Fe:"%RAW_BIN%" ws2_32.lib psapi.lib advapi32.lib
) else (
    where g++.exe >NUL 2>&1
    if not errorlevel 1 (
        g++.exe -std=c++20 -O3 -DNDEBUG -pthread -I"%SCRIPT_DIR%\src" ^
            "%SCRIPT_DIR%\src\common.cpp" ^
            "%SCRIPT_DIR%\src\telemetry.cpp" ^
            "%SCRIPT_DIR%\src\crypto_pake.cpp" ^
            "%SCRIPT_DIR%\src\coordinator.cpp" ^
            "%SCRIPT_DIR%\src\http_server.cpp" ^
            "%SCRIPT_DIR%\src\engine_manager.cpp" ^
            "%SCRIPT_DIR%\src\main.cpp" ^
            "%SCRIPT_DIR%\src\line_editor.cpp" ^
            -o "%RAW_BIN%" -lws2_32 -lpsapi -ladvapi32
    ) else (
        echo [ERROR] Neither MSVC (cl.exe) nor MinGW-w64 (g++.exe) was found in PATH.
        exit /b 1
    )
)

copy /Y "%RAW_BIN%" "%OUT_BIN%" >NUL
del /Q "%RAW_BIN%" 2>NUL

set "BUNDLE_TAR=%CACHE_DIR%\tmp\engines_bundle.tar.gz"
if exist "%BUNDLE_TAR%" del /Q "%BUNDLE_TAR%"
powershell -NoLogo -NoProfile -NonInteractive -Command ^
    "$binDir = '%BIN_DIR%'; $outBin = '%OUT_BIN%'; $tarPath = '%BUNDLE_TAR%'; " ^
    "$items = @(Get-ChildItem -Path $binDir | Where-Object { $_.Name -ne 'lais.exe' -and $_.Name -ne '.lais_bundle_stamp' } | Select-Object -ExpandProperty Name); " ^
    "if ($items.Count -gt 0) { " ^
    "  & tar.exe -czf $tarPath -C $binDir $items; " ^
    "  $offset = (Get-Item $outBin).Length; " ^
    "  $payload = [IO.File]::ReadAllBytes($tarPath); " ^
    "  $trailer = [Text.Encoding]::ASCII.GetBytes(\"`nLAIS_BUNDLE_V1|$offset|$($payload.Length)|END_LAIS_BUNDLE`n\"); " ^
    "  $fs = [IO.File]::Open($outBin, [IO.FileMode]::Append, [IO.FileAccess]::Write); " ^
    "  $fs.Write($payload, 0, $payload.Length); " ^
    "  $fs.Write($trailer, 0, $trailer.Length); " ^
    "  $fs.Close(); " ^
    "  Remove-Item -Force $tarPath -ErrorAction SilentlyContinue; " ^
    "  Write-Host \"  -> Embedded $($items.Count) bundle item(s) into build\\lais.exe\"; " ^
    "}"

copy /Y "%OUT_BIN%" "%BIN_DIR%\lais.exe" >NUL

set "BUILD_ZIP=%BUILD_OUT_DIR%\lais.zip"
set "ROOT_ZIP=%SCRIPT_DIR%\lais.zip"
echo [5/5] Compressing built Windows program, configs, and sources into "%BUILD_ZIP%"...
powershell -NoLogo -NoProfile -NonInteractive -Command ^
    "$stage = Join-Path '%CACHE_DIR%\tmp' 'lais_win_stage\lais'; " ^
    "Remove-Item -Recurse -Force (Split-Path $stage) -ErrorAction SilentlyContinue; " ^
    "New-Item -ItemType Directory -Force -Path (Join-Path $stage 'build') | Out-Null; " ^
    "New-Item -ItemType Directory -Force -Path (Join-Path $stage 'bin') | Out-Null; " ^
    "New-Item -ItemType Directory -Force -Path (Join-Path $stage 'configs') | Out-Null; " ^
    "New-Item -ItemType Directory -Force -Path (Join-Path $stage 'data') | Out-Null; " ^
    "New-Item -ItemType Directory -Force -Path (Join-Path $stage 'src') | Out-Null; " ^
    "Copy-Item -Force '%OUT_BIN%' (Join-Path $stage 'build\lais.exe'); " ^
    "Get-ChildItem -Path '%BIN_DIR%' -File | Where-Object { $_.Name -ne 'lais.exe' -and $_.Name -ne '.lais_bundle_stamp' } | ForEach-Object { Copy-Item -Force $_.FullName (Join-Path $stage 'bin') }; " ^
    "if (Test-Path '%CONFIGS_DIR%') { Get-ChildItem -Path '%CONFIGS_DIR%' -File | Where-Object { $_.Extension -in @('.json','.lang') } | ForEach-Object { Copy-Item -Force $_.FullName (Join-Path $stage 'configs') } }; " ^
    "if (Test-Path '%DATA_DIR%\user.json') { Copy-Item -Force '%DATA_DIR%\user.json' (Join-Path $stage 'data\user.json') }; " ^
    "if (Test-Path 'src') { Get-ChildItem -Path 'src' -File | ForEach-Object { Copy-Item -Force $_.FullName (Join-Path $stage 'src') } }; " ^
    "foreach ($dir in @('tools','tests')) { if (Test-Path $dir) { Copy-Item -Recurse -Force $dir (Join-Path $stage $dir) } }; " ^
    "foreach ($f in @('build.bat','build.sh','CMakeLists.txt','README.md','MODEL_UPDATE_KO.md','LICENSE','.gitignore')) { if (Test-Path $f) { Copy-Item -Force $f $stage } }; " ^
    "if (Test-Path '%BUILD_ZIP%') { Remove-Item -Force '%BUILD_ZIP%' }; " ^
    "Compress-Archive -Path $stage -DestinationPath '%BUILD_ZIP%' -Force; " ^
    "if ('%BUILD_ZIP%' -ne '%ROOT_ZIP%') { Copy-Item -Force '%BUILD_ZIP%' '%ROOT_ZIP%' }; " ^
    "Remove-Item -Recurse -Force (Split-Path $stage) -ErrorAction SilentlyContinue;"

echo ==============================================================================
echo [BUILD ^& BACKUP COMPLETE]
echo   * Windows Binary  : %OUT_BIN% (and %BIN_DIR%\lais.exe)
echo   * JSON Configs    : %CONFIGS_DIR%\{settings.json,sources.json,models.json}
echo   * PAKE User Store : %DATA_DIR%\user.json (Initial ID/PW: admin / admin)
echo   * Backup ZIP File : %BUILD_ZIP%
echo ==============================================================================
endlocal
