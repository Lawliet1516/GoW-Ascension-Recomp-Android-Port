@echo off
rem Builds gowa_discord.exe: the Discord Rich Presence + "Join" helper the launcher starts while online play is on
rem ("Show on Discord" on the Online page). The launcher looks for it in ascension\discord\gowa_discord.exe.
rem Needs: Visual Studio 2022 Build Tools (clang-cl) and the Discord Social SDK (discord.com/developers, Social SDK
rem download; tested with 1.10.19337). Set DISCORD_SDK to its discord_social_sdk folder, or unpack it next to this file.
setlocal
if "%DISCORD_SDK%"=="" set DISCORD_SDK=%~dp0discord_social_sdk
if not exist "%DISCORD_SDK%\include" (echo Discord Social SDK not found at %DISCORD_SDK% & exit /b 1)
if "%VSDIR%"=="" set VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
set OUT=%~dp0..\discord
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
"%VSDIR%\VC\Tools\Llvm\x64\bin\clang-cl.exe" /nologo /std:c++17 /EHsc /O2 /MD ^
  /I "%DISCORD_SDK%\include" "%~dp0gowa_discord.cpp" /Fe"%OUT%\gowa_discord.exe" /Fo"%TEMP%\gowa_discord.obj" ^
  /link /LIBPATH:"%DISCORD_SDK%\lib\release" discord_partner_sdk.lib winhttp.lib || exit /b 1
copy /y "%DISCORD_SDK%\bin\release\discord_partner_sdk.dll" "%OUT%\" >nul || exit /b 1
echo built %OUT%\gowa_discord.exe
