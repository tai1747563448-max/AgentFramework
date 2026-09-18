@echo off
call "E:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "E:\desktop\How_to_build_a_agent\AgentFramework-latency\build\vs2022"
cmake . -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release --target agent_runtime agent_kernel -j 4
cmake --build . --config Release --target task_type_tests interactive_cli_tests -j 4
ctest -C Release --output-on-failure -R "task_type_tests|interactive_cli_tests"
exit /b %errorlevel%
