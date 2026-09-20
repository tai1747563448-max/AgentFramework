@echo off
REM Fresh-configure path: wipes build/vs2022/CMakeFiles/rules.ninja, so run
REM scripts\_session_build.cmd afterwards to restore the deps-prefix patch and
REM rebuild from scratch (see docs/test-baseline-2026-09-20.md).
REM
REM Set git mirror for github.com before invoking cmake so FetchContent's
REM internal git clones route through the China-accessible ghfast.top
REM proxy. The mirror is read-only equivalent for public repos.
git config --global url."https://ghfast.top/https://github.com/".insteadOf "https://github.com/"
rem Root derived from this script's location, so it configures whichever
rem worktree it is invoked from.
for %%I in ("%~dp0..") do set "REPO_ROOT=%%~fI"
call "E:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /D "%REPO_ROOT%\build\vs2022"
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S "%REPO_ROOT%" -B "%REPO_ROOT%\build\vs2022" %*

