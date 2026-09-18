@echo off
cd /d "E:\desktop\How_to_build_a_agent\AgentFramework-latency\build\vs2022"
echo === runtime_engine_tests ===
runtime_engine_tests.exe
echo.
echo === terminal_presenter_tests ===
terminal_presenter_tests.exe
echo.
echo === session_engine_tests ===
session_engine_tests.exe
exit /b %errorlevel%