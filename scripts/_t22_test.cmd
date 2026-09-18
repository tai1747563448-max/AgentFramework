@echo off
call "E:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "E:\desktop\How_to_build_a_agent\AgentFramework-latency\build\vs2022"
cmake --build . --config Release --target sandbox_profile_tests sandbox_factory_tests bubblewrap_sandbox_tests direct_process_runner_tests -j 4
ctest -C Release --output-on-failure -R "sandbox_profile_tests|sandbox_factory_tests|bubblewrap_sandbox_tests|direct_process_runner_tests"
exit /b %errorlevel%
