@echo off
rem Session helper: patch the generated ninja rules, then rebuild the project.
rem
rem Two environment quirks are handled here. Both fail SILENTLY on their own,
rem which is why they are worth this much ceremony:
rem
rem 1. scripts/patch_ninja_deps_prefix.py repairs the msvc_deps_prefix line in
rem    CMakeFiles\rules.ninja. CMake detects cl.exe's /showIncludes prefix by
rem    decoding probe output as UTF-8, but this machine's MSVC is
rem    Chinese-localised and prints the prefix in GBK. CMake's decoded prefix
rem    therefore contains U+FFFD and never matches, so ninja records ZERO
rem    header dependencies (verify with `ninja -t deps <obj>`: "#deps 0").
rem    The build still succeeds, but editing a header leaves every object that
rem    includes it stale, and the resulting binary mixes two layouts of the
rem    same struct - which surfaces as heap corruption or bad_variant_access
rem    in a test unrelated to the header. That is the whole "flaky test /
rem    baseline drift" story; see docs/test-baseline-2026-09-20.md.
rem
rem    The patch lives in a generated file, so `cmake` re-configure wipes it.
rem    When the patcher reports that it had to rewrite the line (exit 2), any
rem    object files already on disk were compiled with dependency tracking
rem    OFF and cannot be trusted, so we drop our own object graph and let the
rem    build recompile from scratch. That costs a full compile once per
rem    reconfigure and is the only safe response.
rem
rem 2. Builds use `-k 0`. The curl docs rule shells out to a generated .bat
rem    that calls perl; the perl on PATH is the msys one (/usr/bin/perl),
rem    which cmd.exe cannot resolve, so a bare `cmake --build .` aborts the
rem    whole ninja graph on that one rule. Docs generation is pure man-page
rem    text and is not an input to any test binary, so we let it fail and
rem    keep building the rest.
rem
rem The link rules are left as CMake generated them: `cmake -E vs_link_exe`
rem works fine as long as the build runs from a shell where vcvars64.bat has
rem been loaded, which is what this script does. The older workaround that
rem replaced the link rules with a direct link.exe call compensated for
rem running ninja without vcvars64, which cannot work anyway (LNK1181 cannot
rem find ws2_32.lib), and is no longer needed.
setlocal
rem Derive the repository root from this script's own location, so the same
rem helper drives any worktree: AgentFramework, AgentFramework-latency, or the
rem iteration worktrees under .worktrees/. A hard-coded root here used to make
rem the script build the latency worktree even when run from the main checkout,
rem which is a silent wrong-tree build - the worst kind.
for %%I in ("%~dp0..") do set "REPO_ROOT=%%~fI"
set "BUILD_DIR=%REPO_ROOT%\build\vs2022"
if not exist "%BUILD_DIR%\build.ninja" (
    echo [build] "%BUILD_DIR%" is not a Ninja build tree.
    echo [build] Configure it first: "%REPO_ROOT%\scripts\configure_msvc.cmd"
    exit /b 1
)
call "E:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%REPO_ROOT%"
python "%REPO_ROOT%\scripts\patch_ninja_deps_prefix.py" "%BUILD_DIR%\CMakeFiles\rules.ninja"
set PATCH_RC=%ERRORLEVEL%
if "%PATCH_RC%"=="1" exit /b 1
if "%PATCH_RC%"=="2" (
    echo [build] rules.ninja deps prefix was regenerated; dropping stale objects
    for /d %%D in ("%BUILD_DIR%\CMakeFiles\*.dir") do rmdir /s /q "%%D"
)
cd /d "%BUILD_DIR%"
set BUILD_LOG=%REPO_ROOT%\build\_session_build.log
cmake --build . --config Release -j 4 -- -k 0 > "%BUILD_LOG%" 2>&1
set BUILD_RC=%ERRORLEVEL%
type "%BUILD_LOG%"
if "%BUILD_RC%"=="0" exit /b 0
rem `-k 0` returns non-zero whenever ANY edge failed, and the vendored curl docs
rem rule always fails here - so the exit code alone cannot distinguish a real
rem compile/link error from the expected docs noise. Check for ninja FAILED
rem edges instead, ignoring the curl docs ones: silently reporting test results
rem from a half-built tree is exactly the failure mode this script exists to
rem prevent. (A bare "error C"/"LNK" grep is not enough - a rule can fail
rem without either, e.g. the shell-level 255 that vs_link_exe produced.)
findstr /c:"FAILED:" "%BUILD_LOG%" | findstr /v /c:"curl-build" | findstr /r "^." >nul
if not errorlevel 1 (
    echo [build] FAILED - see "%BUILD_LOG%"; not running tests.
    exit /b 1
)
echo [build] only the vendored curl docs rule failed; continuing.
exit /b 0
