@echo off
rem Build the agent_ready_package target: copies AgentFramework.exe, its
rem runtime DLLs and the local .env into out/AgentFramework-Ready/ via
rem cmake/stage_ready_package.cmake. Use this after changing .env, otherwise
rem the copy in the Ready directory stays stale.
rem
rem The previous version of this script pointed cmake at a hard-coded
rem Python-site-packages path and wrapped the build in `cmake -E chdir`, which
rem left `--build .` pointing at the source tree instead of the build tree.
rem Plain `cmake` off PATH plus the already-current working directory works.
setlocal
rem Root derived from this script's location, so it works from any worktree.
for %%I in ("%~dp0..") do set "REPO_ROOT=%%~fI"
call "E:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%REPO_ROOT%\build\vs2022"
cmake --build . --config Release --target agent_ready_package
exit /b %errorlevel%
