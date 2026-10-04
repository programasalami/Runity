-- One-time local setup, run as the PostgreSQL superuser (it asks for the password chosen when PostgreSQL was installed):
--
--   "C:\Program Files\PostgreSQL\17\bin\psql.exe" -h localhost -U postgres -d postgres -v runity_password="'runitypass'" -f Database\setup\create_database.sql
--
-- It creates the login runity and the databases runity and runity_test (the tests use runity_test). runitypass is the
-- local development password both services are configured with (AccountService appsettings.json PgConnInfo, Server
-- config/gameserver.json pgConnInfo); a server others can reach uses its own password there. The Account/API service creates
-- the tables when it starts. The reference's own database ("alloy") is not touched.

\set ON_ERROR_STOP on

CREATE ROLE runity LOGIN PASSWORD :runity_password;
CREATE DATABASE runity OWNER runity;
CREATE DATABASE runity_test OWNER runity;   -- integration tests create and drop their own tables here
