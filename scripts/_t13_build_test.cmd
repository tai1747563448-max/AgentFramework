@echo off
call "E:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "E:\desktop\How_to_build_a_agent\AgentFramework-latency"
cmake --build build\vs2022 --config Release --target hook_chain_tests -j 4
if errorlevel 1 exit /b 1
cd build\vs2022
hook_chain_tests.exe
exit /b %errorlevel%