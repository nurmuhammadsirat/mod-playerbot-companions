# Playerbot Companions

Version 0.1 lets each player character keep a persistent roster of existing playerbots. Bring a bot into
your party through Playerbots, then recruit it with `.companion add`. The roster lives in its own MySQL
database and survives worldserver restarts.

## Dependencies

- AzerothCore WotLK on the `Playerbot` branch, with `mod-playerbots` installed and enabled.
- Static builds for both modules (`-DMODULES=static` with the default per-module settings).
- MySQL and the MySQL command-line client for automatic database setup.

`mod-llm-chatter` is optional. `mod-solo-party-xp` can be used alongside this module and continues
to calculate XP from actual group membership.

## Installation

1. Put this directory in `modules/mod-playerbot-companions/` in your AzerothCore checkout.
2. From the AzerothCore root, reconfigure and build using your existing build directory and install prefix.

   ```sh
   cmake -S . -B build -DMODULES=static
   cmake --build build --target worldserver --parallel 4
   ```

3. Stop worldserver and install it using your normal AzerothCore deployment process. In an existing
   complete build, `cmake --install build` installs the binary and the module configuration template.
4. Copy `mod_playerbot_companions.conf.dist` to `mod_playerbot_companions.conf` in the installed
   `etc/modules/` directory. Check the database settings below before starting worldserver.
5. Start worldserver. The module updater creates or updates the schema using the SQL files in
   `data/sql/playerbot_companions/`. Keep those files in the configured AzerothCore source checkout
   while automatic updates are enabled.

## Database configuration

- `PlayerbotCompanions.DatabaseInfo` defaults to an empty value. This reuses the host, port, and
  credentials from `CharacterDatabaseInfo` with the database name `acore_playerbot_companions`.
  To supply a separate connection, use `host;port;user;password;database`.
- `PlayerbotCompanions.DatabaseUpdates` defaults to `1` and applies the module’s SQL migrations at
  startup. Automatic creation of a missing database also requires `Updates.AutoSetup = 1`.

If the configured MySQL account lacks permission to create the database, have a MySQL administrator
create `acore_playerbot_companions` and grant the account access to that database. Replace
`'acore'@'localhost'` with the existing MySQL account used by the module.

```sql
CREATE DATABASE IF NOT EXISTS `acore_playerbot_companions` DEFAULT CHARACTER SET utf8mb4;
GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, ALTER ON `acore_playerbot_companions`.*
  TO 'acore'@'localhost';
```

For manual setup, create the dedicated database and run the files in
`data/sql/playerbot_companions/updates/` against it in filename order. Set
`PlayerbotCompanions.DatabaseUpdates = 0` and grant the configured account `SELECT`, `INSERT`,
`UPDATE`, and `DELETE` on the companion table. Use a different companion database for each realm
that has its own character database, and back up the two databases together.

Worldserver stops during startup if it cannot open or initialize the companion database.

## In-game commands

- `.companion add [name]` recruits an online bot in your group. Omit the name to use your selected target.
- `.companion list` shows this character’s recruits and whether each is in your group, online as a bot, or unavailable.
- `.companion dismiss [name]` removes a recruited bot from your current party or raid. Omit the name to
  use your selected target. You need normal group removal permissions.
- `.companion remove <name or character ID>` erases the relationship, even if the bot is offline.
  Use the displayed numeric ID if a listed character has become unavailable. The bot can stay in your party.

## In-game verification

1. Log in on a manually controlled character. Run `.companion list` and check that your roster is empty.
2. Add or invite an existing bot using Playerbots. Select it and run `.companion add` twice. Verify that
   `.companion list` shows the bot in your group exactly once. Try recruiting yourself or a human party
   member and verify that the command declines.
3. As group leader, run `.companion dismiss BotName`. Verify that the bot leaves the group and still
   appears in your roster. In a two-member party, expect the ordinary party disband.
4. Restart worldserver, log back in on the same character, and verify that `.companion list` still
   shows the bot. Log in on a different character and check that its roster is separate.
5. Run `.companion remove BotName` while the bot is offline. Check that the record is gone and stays
   gone after a restart. If you recruit and remove a bot while it is still in your party, verify that the
   bot remains in the party.

## Limitations

- The roster is per character. Random bots remain in Playerbots’ shared pool and can appear on multiple
  rosters. To bring a companion back into your party, use normal Playerbots login and invitation flow.
- Dismissal applies to ordinary parties and raids. Dungeon Finder, battleground, and battlefield groups
  use their own rules. A bot played by a person, including SelfBot characters, is ineligible.
- A deleted or otherwise unavailable character can leave a roster entry behind. Use its displayed ID
  to clear it. Character creation times protect saved relationships if AzerothCore reuses a numeric ID.
