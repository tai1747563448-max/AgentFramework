@echo off
call "E:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "E:\desktop\How_to_build_a_agent\AgentFramework-latency\build\vs2022"
cmake --build . --config Release --target stop_reason_codec_tests runtime_engine_tests state_reducer_tests anthropic_adapter_tests jsonl_event_store_tests model_memory_consolidator_tests session_engine_tests task_evaluator_tests -j 4
ctest -C Release --output-on-failure -R "stop_reason_codec_tests|runtime_engine_tests|state_reducer_tests|anthropic_adapter_tests|jsonl_event_store_tests|model_memory_consolidator_tests|session_engine_tests|task_evaluator_tests"
exit /b %errorlevel%
