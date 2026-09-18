@echo off
call "E:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /D "E:\desktop\How_to_build_a_agent\AgentFramework-latency\build\vs2022"
REM Bypass broken vs_link_exe wrapper: link agent.exe ourselves, then
REM let ninja build everything else.
"E:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.37.32822\bin\Hostx64\x64\link.exe" /nologo CMakeFiles\agent.dir\src\main.cpp.obj /out:AgentFramework.exe /implib:AgentFramework.lib /pdb:AgentFramework.pdb /version:0.0 /machine:x64 /INCREMENTAL:NO /subsystem:console agent_runtime.lib agent_kernel.lib _deps\cpr-build\cpr\cpr.lib _deps\curl-build\lib\libcurl_imp.lib ws2_32.lib bcrypt.lib kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib
if errorlevel 1 exit /b %errorlevel%
copy /Y _deps\cpr-build\cpr\cpr.dll . >nul
copy /Y _deps\curl-build\lib\libcurl.dll . >nul
cmake --build . --config Release %*