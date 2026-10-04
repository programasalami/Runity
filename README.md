# Unity Client + C++ Server

## Credits

Credit to **Link & Zolmex** for their awesome work and creation of **Alloy**. They created the entire foundation that this port was built around.

* [Alloy Client](https://github.com/NotTheLegend/Alloy-Client)
* [Alloy Server](https://github.com/Zolmex/Alloy-Server)

## Navigation

| Directory                  | Description                                                                                             |
| -------------------------- | ------------------------------------------------------------------------------------------------------- |
| `Documentation/Migration/` | The design of the new system and the migration status (8 documents)                                     |
| `Protocol/`                | The game protocol contract (`schema/protocol.toml`), its generator, and shared test vectors             |
| `Content/`                 | Gameplay definitions, maps, and world configs, read by both the server and client (one source of truth) |
| `Server/`                  | The C++23 game server (CMake + vcpkg), headless                                                         |
| `AccountService/`          | The C# ASP.NET Core Account/API service                                                                 |
| `Database/`                | PostgreSQL migrations and the one-time setup script                                                     |
| `Runity/`                  | The Unity 6 client project                                                                              |
| `Tests/`                   | `ClientCore` (the client's engine-free code under .NET) and `EndToEnd` (real processes)                 |

## Getting Started

**Requirements:** Windows, Visual Studio 2026 (or its Build Tools) with "Desktop development with C++", .NET 10 SDK, Python 3,
Unity 6000.6.4f1, Memurai (Redis for Windows) and PostgreSQL 17.

1. **Database (once):** with Memurai and PostgreSQL installed and running, run `Database/setup/create_database.sql` as the
   `postgres` superuser (the command is at the top of the file). It creates the login `runity` / `runitypass` that both servers are
   configured with, and the databases `runity` and `runity_test`. That is a local development password; a server others can reach
   should use its own, set in `appsettings.json` (`PgConnInfo`) and `gameserver.json` (`pgConnInfo`). The account server creates
   the tables the first time it starts.
2. **Game server:** `Server\build.cmd`. The first build downloads and compiles its libraries through vcpkg, which takes a while.
3. **Account server:** `dotnet build AccountService`.
4. **Game client:** in Unity Hub, **Add > Add project from disk** and choose the `Runity` folder (Unity 6000.6.4f1; the first open
   imports for a few minutes). Then **Runity > Build Windows Player** builds `Builds\Windows\Runity.exe`.
5. **Play:** start the three programs, servers first:

   | Program | Where |
   |---|---|
   | Account server | `AccountService\src\Runity.AccountService\bin\Debug\net10.0\Runity.AccountService.exe` |
   | Game server | `Server\build\debug\app\runity_gameserver.exe` |
   | Game | `Builds\Windows\Runity.exe` (or press **Play** on the `Game` scene in Unity) |

   Register, create a character, then: **WASD** move, mouse aims and fires, **Q/E** turn, **F** enters a portal, **R** returns to
   the Nexus, **Enter** chats. **F4** opens the developer console, **F5** shows the developer stats. Accounts and characters are
   saved in PostgreSQL.

## Tests

```bash
Server\build.cmd                           # C++ server: build and run its tests
dotnet test AccountService                 # Account/API service
dotnet test Tests/ClientCore               # Unity client code + EditMode tests, outside Unity
dotnet test Tests/EndToEnd                 # Everything together (needs Memurai, PostgreSQL and a built game server)
```

In Unity, the Test Runner runs the **EditMode** and **PlayMode** tests. After editing `Protocol/schema/protocol.toml`, run
`python Protocol/generator/gen.py` to regenerate the C++ and C# protocol code.
