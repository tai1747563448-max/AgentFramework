@echo off
setlocal
chcp 65001 >nul
call "E:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "E:\desktop\How_to_build_a_agent\AgentFramework-latency\build\vs2022"
"C:\Users\Lenovo\AppData\Local\Programs\Python\Python311\Lib\site-packages\cmake\data\bin\cmake.exe" -E chdir "E:/desktop/How_to_build_a_agent/AgentFramework-latency" ^
    "C:\Users\Lenovo\AppData\Local\Programs\Python\Python311\Lib\site-packages\cmake\data\bin\cmake.exe" --build . --config Release --target agent_ready_package
exit /b %errorlevel%