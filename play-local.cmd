@echo off
rem Warriors & Wizards - play on this PC.
rem Starts the Account/API service (accounts kept in memory: they reset when it closes), the C++ game server and the game.
rem Needs: Redis / Memurai running on 127.0.0.1:6379 (it is a Windows service here), .NET 10, and a built game
rem (Builds\Windows\WaW.exe - otherwise open the WaW project in Unity, scene Game, and press Play).
rem Close the two server windows to stop.
setlocal
cd /d "%~dp0"

if not exist Server\build\debug\app\waw_gameserver.exe (
    echo Building the game server...
    call Server\build.cmd || goto :failed
)
echo Building the account service...
dotnet build AccountService\src\WaW.AccountService -v q -nologo || goto :failed

start "WaW Account Service" /D "%~dp0AccountService\src\WaW.AccountService\bin\Debug\net10.0" dotnet WaW.AccountService.dll --urls http://127.0.0.1:5080 --Service:AccountStore=InMemory
start "WaW Game Server" /D "%~dp0Server" build\debug\app\waw_gameserver.exe --config config/gameserver.json

echo Waiting for the servers to start...
timeout /t 6 /nobreak >nul

if exist Builds\Windows\WaW.exe (
    start "" "%~dp0Builds\Windows\WaW.exe"
) else (
    echo No game build yet: open the WaW project in Unity, open the Game scene and press Play.
)
echo.
echo Create an account in the game (any name of 1-10 letters, a password of 9+ characters), create a character and play.
echo Controls: WASD move, mouse aims and fires, Q/E turn the camera, F enters a portal, R returns to the Nexus, Enter chats.
exit /b 0

:failed
echo Something failed - see the messages above.
pause
exit /b 1
