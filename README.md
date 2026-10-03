# Credits to Link & Zolmex for their awesome work and creation of Alloy, they created the entire foundation of what this port was built around.
https://github.com/NotTheLegend/Alloy-Client
https://github.com/Zolmex/Alloy-Server

# Unity client + C++ server

| Navigation | 
| --- | --- |
| `Documentation/Migration/` | the audit of the reference and the design of the new system (19 documents) |
| `Protocol/` | the game protocol contract (`schema/protocol.toml`), its generator and shared test vectors |
| `Content/` | the gameplay definitions, maps and world configs, read by the server AND the client (one source of truth) |
| `Server/` | the C++23 game server (CMake + vcpkg), headless |
| `AccountService/` | the C# ASP.NET Core Account/API service |
| `Database/` | PostgreSQL migrations and the one-time setup script |
| `WaW/` | the Unity 6 client project |
| `Tests/` | `ClientCore` (the client's engine-free code under .NET) and `EndToEnd` (real processes) |

## Build and test

```
python Protocol/generator/gen.py           # after editing Protocol/schema/protocol.toml
Server\build.cmd                           # C++ server: configure, build, run the tests (Visual Studio 2026 toolchain)
dotnet test AccountService                 # Account/API service
dotnet test Tests/ClientCore               # Unity client code + EditMode tests, outside Unity
dotnet test Tests/EndToEnd                 # everything together (needs Redis on 127.0.0.1:6379 and a built server)
```
Unity: open `WaW/`, run `WaW > Build Game Scene` once, then use the Test Runner (EditMode / PlayMode).

## Run locally (development)

1. Redis (Memurai) on 127.0.0.1:6379.
2. Account/API service without a database (accounts are kept in memory):
   `dotnet run --project AccountService/src/WaW.AccountService -- --Service:AccountStore=InMemory`
   With PostgreSQL: run `Database/setup/create_database.sql` once, set `WAW_PG_CONNINFO`, then `... -- migrate` and run without the flag.
3. Game server: `cd Server && build\debug\app\waw_gameserver.exe --config config/gameserver.json`
4. Unity: open the `Game` scene and press Play (sign in, create a character, WASD to move, Q/E to turn, Enter to chat).
