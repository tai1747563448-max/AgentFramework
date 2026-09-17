@echo off
setlocal
call "E:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /D "E:\desktop\How_to_build_a_agent\AgentFramework-latency\build\vs2022"
set "TARGET=%~1"
if "%TARGET%"=="" (
    echo Usage: link_test.cmd ^<target_name^>
    exit /b 1
)
set "OBJS=CMakeFiles\%TARGET%.dir\tests\test_main.cpp.obj"
if exist "CMakeFiles\%TARGET%.dir\tests\adapters" dir /b "CMakeFiles\%TARGET%.dir\tests\adapters" > nul
for /r "CMakeFiles\%TARGET%.dir\tests" %%f in (*.cpp.obj) do (
    set "OBJS=!OBJS! %%f"
)
echo Linking %TARGET%...
link.exe /nologo !OBJS! /out:%TARGET%.exe /pdb:%TARGET%.pdb /machine:x64 /INCREMENTAL:NO /subsystem:console agent_kernel.lib _deps\cpr-build\cpr\cpr.lib _deps\curl-build\lib\libcurl_imp.lib ws2_32.lib kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib
endlocal
