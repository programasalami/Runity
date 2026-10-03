-- One-time local setup for the NEW stack (run by the project owner as the PostgreSQL superuser).
-- It creates a dedicated login role and two databases. The reference's own database ("alloy") is not touched.
--
--   "C:\Program Files\PostgreSQL\17\bin\psql.exe" -h localhost -U postgres -d postgres ^
--       -v waw_password="'choose-a-password'" -f Database\setup\create_database.sql
--
-- Then create Runity\.env.local (git-ignored) with:
--   WAW_PG_CONNINFO=host=localhost port=5432 dbname=waw user=waw password=<the password>
--   WAW_PG_TEST_CONNINFO=host=localhost port=5432 dbname=waw_test user=waw password=<the password>
--   WAW_REDIS_URL=redis://127.0.0.1:6379
-- and apply the schema:  dotnet run --project AccountService/src/WaW.AccountService -- migrate

\set ON_ERROR_STOP on

CREATE ROLE waw LOGIN PASSWORD :waw_password;
CREATE DATABASE waw OWNER waw;
CREATE DATABASE waw_test OWNER waw;   -- integration tests create and drop their own tables here
