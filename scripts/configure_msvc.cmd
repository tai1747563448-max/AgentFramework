@echo off
REM Set git mirror for github.com before invoking cmake so FetchContent's
REM internal git clones route through the China-accessible ghfast.top
REM proxy. The mirror is read-only equivalent for public repos.
git config --global url."https://ghfast.top/https://github.com/".insteadOf "https://github.com/"
call "E:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /D "E:\desktop\How_to_build_a_agent\AgentFramework-latency\build\vs2022"
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S "E:\desktop\How_to_build_a_agent\AgentFramework-latency" -B "E:\desktop\How_to_build_a_agent\AgentFramework-latency\build\vs2022" %*
