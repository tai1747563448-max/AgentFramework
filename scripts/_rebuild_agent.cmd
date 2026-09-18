@echo off
chcp 65001 >nul
call "E:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "E:\desktop\How_to_build_a_agent\AgentFramework-latency"
cmake --build build\vs2022 --config Release --target agent -j 4
exit /b %errorlevel%