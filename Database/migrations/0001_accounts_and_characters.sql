-- 0001: accounts and characters (normalized).
-- Ownership (Architecture.md section 5): accounts = Account/API service; characters = GameServer, except that the API service
-- may soft-delete a character with a single conditional statement.

CREATE TABLE accounts (
    id               BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    name             TEXT        NOT NULL,
    name_key         TEXT        NOT NULL GENERATED ALWAYS AS (lower(name)) STORED,
    password_hash    TEXT        NOT NULL,                 -- self-describing: pbkdf2-sha256$<iterations>$<salt b64>$<hash b64>
    registration_ip  TEXT        NOT NULL DEFAULT '',
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_login_at    TIMESTAMPTZ,
    rank             SMALLINT    NOT NULL DEFAULT 0,       -- 0 player, 80 moderator, 90 developer, 100 owner (reference Ranks)
    gold             BIGINT      NOT NULL DEFAULT 0 CHECK (gold >= 0),
    fame             BIGINT      NOT NULL DEFAULT 0 CHECK (fame >= 0),
    total_fame       BIGINT      NOT NULL DEFAULT 0,
    max_characters   SMALLINT    NOT NULL DEFAULT 2 CHECK (max_characters >= 0),
    next_character_id INT        NOT NULL DEFAULT 1,
    starter_pending  BOOLEAN     NOT NULL DEFAULT TRUE,
    CONSTRAINT accounts_name_rule CHECK (name ~ '^[A-Za-z]{1,10}$')
);
CREATE UNIQUE INDEX accounts_name_key ON accounts (name_key);

CREATE TABLE characters (
    account_id       BIGINT      NOT NULL REFERENCES accounts (id) ON DELETE CASCADE,
    character_id     INT         NOT NULL,
    class_type       INT         NOT NULL,
    skin_type        INT         NOT NULL DEFAULT 0,
    level            INT         NOT NULL DEFAULT 1 CHECK (level >= 1),
    xp               BIGINT      NOT NULL DEFAULT 0 CHECK (xp >= 0),
    fame             BIGINT      NOT NULL DEFAULT 0 CHECK (fame >= 0),
    hp               INT         NOT NULL DEFAULT 0,
    mp               INT         NOT NULL DEFAULT 0,
    stats            INT[]       NOT NULL DEFAULT '{}',    -- base stats: max HP, max MP, att, def, spd, dex, vit, wis (the original CharacterStats)
    items            INT[]       NOT NULL DEFAULT '{}',    -- one entry per player slot, -1 = empty (reference ItemTypes)
    health_potions   SMALLINT    NOT NULL DEFAULT 1,       -- the original's potion stacks (newCharsConfig HealthPotions / MagicPotions)
    magic_potions    SMALLINT    NOT NULL DEFAULT 1,
    has_backpack     BOOLEAN     NOT NULL DEFAULT FALSE,
    extra            JSONB       NOT NULL DEFAULT '{}',    -- document-shaped, rarely queried: combat / kill / exploration stats
    is_dead          BOOLEAN     NOT NULL DEFAULT FALSE,
    is_deleted       BOOLEAN     NOT NULL DEFAULT FALSE,
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    save_version     BIGINT      NOT NULL DEFAULT 0,       -- increases with every save; a save carrying an older version is refused
    PRIMARY KEY (account_id, character_id)
);
CREATE INDEX characters_alive ON characters (account_id) WHERE NOT is_dead AND NOT is_deleted;
