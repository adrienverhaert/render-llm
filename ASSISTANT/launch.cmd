@echo off
setlocal
cd /d "%~dp0"
set "AGENT_DIR=%~1"
del /q "%AGENT_DIR%\connect_error.txt" >nul 2>&1
where node >nul 2>&1
if errorlevel 1 (
  > "%AGENT_DIR%\connect_error.txt" echo Agent could not start: Node.js 20 or newer was not found.
  >> assistant_launch.log echo Agent could not start: Node.js 20 or newer was not found.
  exit /b 1
)

REM Apps opened from Explorer may not inherit Codex Desktop's private CLI path.
set "CODEX_CLI_PATH="
for /f "delims=" %%C in ('where codex 2^>nul') do if not defined CODEX_CLI_PATH set "CODEX_CLI_PATH=%%C"
if not defined CODEX_CLI_PATH (
  for /d %%D in ("%LOCALAPPDATA%\OpenAI\Codex\bin\*") do if exist "%%~fD\codex.exe" set "CODEX_CLI_PATH=%%~fD\codex.exe"
)
if not defined CODEX_CLI_PATH (
  > "%AGENT_DIR%\connect_error.txt" echo Agent could not start: install Codex Desktop and sign in with ChatGPT.
  >> assistant_launch.log echo Agent could not start: Codex Desktop was not found.
  exit /b 1
)
if not exist "node_modules\@anthropic-ai\claude-agent-sdk\package.json" (
  > "%AGENT_DIR%\connect_error.txt" echo Agent could not start: its packaged runtime dependencies are missing.
  >> assistant_launch.log echo Agent could not start: packaged runtime dependencies are missing.
  exit /b 1
)
if not exist "dist\index.js" (
  > "%AGENT_DIR%\connect_error.txt" echo Agent could not start: its packaged runtime is missing.
  >> assistant_launch.log echo Agent could not start: packaged runtime is missing.
  exit /b 1
)
REM %~3 is the reasoning effort. Older callers omit it and config.ts uses high.
if "%~3"=="" ( start "" /b node "dist\index.js" --dir "%~1" --model "%~2" ) else ( start "" /b node "dist\index.js" --dir "%~1" --model "%~2" --effort "%~3" )
endlocal
