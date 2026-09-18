@echo off
call "E:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "E:\desktop\How_to_build_a_agent\AgentFramework-latency"
cmake -S . -B build/vs2022 -G Ninja
exit /b %errorlevel%