# Warriors & Wizards — Unity Client + C++ Server

## Credits

Credit to **Link & Zolmex** for their awesome work and creation of **Alloy**. They created the entire foundation that this port was built around.

* [Alloy Client](https://github.com/NotTheLegend/Alloy-Client)
* [Alloy Server](https://github.com/Zolmex/Alloy-Server)

## Navigation

| Directory                  | Description                                                                                             |
| -------------------------- | ------------------------------------------------------------------------------------------------------- |
| `Documentation/Migration/` | The audit of the reference and the design of the new system (19 documents)                              |
| `Protocol/`                | The game protocol contract (`schema/protocol.toml`), its generator, and shared test vectors             |
| `Content/`                 | Gameplay definitions, maps, and world configs, read by both the server and client (one source of truth) |
| `Server/`                  | The C++23 game server (CMake + vcpkg), headless                                                         |
| `AccountService/`          | The C# ASP.NET Core Account/API service                                                                 |
| `Database/`                | PostgreSQL migrations and the one-time setup script                                                     |
| `WaW/`                     | The Unity 6 client project                                                                              |
| `Tests/`                   | `ClientCore` (the client's engine-free code under .NET) and `EndToEnd` (real processes)                 |

## Build and Test

```bash
python Protocol/generator/gen.py           # Run after editing Protocol/schema/protocol.toml
Server\build.cmd                           # C++ server: configure, build, and run tests (Visual Studio 2026 toolchain)
dotnet test AccountService                 # Account/API service
dotnet test Tests/ClientCore               # Unity client code + EditMode tests, outside Unity
dotnet test Tests/EndToEnd                 # Everything together (requires Redis on 127.0.0.1:6379 and a built server)
```

### Unity

Open `WaW/`, run **`WaW > Build Game Scene`** once, then use the Test Runner:

* **EditMode**
* **PlayMode**

## Run Locally (Development)

1. **Redis** — Run Memurai on `127.0.0.1:6379`.

2. **Account/API service** — Run without a database (accounts are kept in memory):

   ```bash
   dotnet run --project AccountService/src/WaW.AccountService -- --Service:AccountStore=InMemory
   ```

   To use PostgreSQL instead, run `Database/setup/create_database.sql` once, set `WAW_PG_CONNINFO`, then run:

   ```bash
   ... -- migrate
   ```

   After migration, run the service without the `--Service:AccountStore=InMemory` flag.

3. **Game server** — Start the server:

   ```bash
   cd Server
   build\debug\app\waw_gameserver.exe --config config/gameserver.json
   ```

4. **Unity** — Open the `Game` scene and press **Play**.

   * Sign in
   * Create a character
   * Use **WASD** to move
   * Use **Q/E** to turn
   * Press **Enter** to chat

