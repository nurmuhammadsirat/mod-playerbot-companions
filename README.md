# Playerbot Companions

Each player character can keep a persistent roster of existing playerbots. Bring a bot into your party
through Playerbots, then recruit it with `.companion add`. Your roster, loyalty, journal, saved
lineups, and outing requests live in a dedicated MySQL database and survive worldserver restarts.

## DISCLAIMER

This module is made with AI assistance as I have zero c++ experience.

## Dependencies

- AzerothCore WotLK on the `Playerbot` branch, with `mod-playerbots` installed and enabled.
- Static builds for both modules (`-DMODULES=static` with the default per-module settings).
- MySQL and the MySQL command-line client for automatic database setup.

### Optional integration

[mod-llm-chatter](../mod-llm-chatter/README.md#optional-integration) can use companion loyalty and
shared memories in party replies. These commits provide the schema and bridge support.

- `mod-playerbot-companions` — `a29baabf205d4f415299fc9a518803c1f98afdf6` adds the shared-memory schema.
- `mod-llm-chatter` — `4687ca28e5a0637fff0a43f9f87ecb8d00501c54` adds support for including provider context in prompts.

Use a bridge with `LLMChatter.AdditionalContext` support and set these values in its
`mod_llm_chatter.conf`.

```ini
LLMChatter.AdditionalContext = 1
LLMChatter.AdditionalContext.Provider = /path/to/mod-playerbot-companions/tools/party_context.py
LLMChatter.AdditionalContext.DatabaseInfo = localhost;3306;context_reader;PASSWORD;acore_playerbot_companions
```

Use a provider path visible from the bridge process. Set the connection to the same companion
database that worldserver uses. Grant the bridge's context account `SELECT` on
`playerbot_companions` and `playerbot_companion_memories`. On upgrades, replace the old
`LLMChatter.Companions.*` settings with the three settings above and restart the bridge.

The provider uses the current loyalty tier and recent activity to guide a bot's tone, including
when a long absence makes a reunion feel more reserved. It selects up to three memories with
priority for the current map, including relevant milestone and tradition summaries. Those facts
belong to the addressed bot, or the primary bot in a multi-bot reply. Pinning changes the in-game
journal view. Outing suggestions are delivered privately by worldserver.

`mod-solo-party-xp` can be used alongside this module and continues to calculate XP from actual group membership.

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
`data/sql/playerbot_companions/updates/` against it in filename order. When upgrading an existing
installation, apply only migrations that have yet to run. Set
`PlayerbotCompanions.DatabaseUpdates = 0` and grant the configured account `SELECT`, `INSERT`,
`UPDATE`, and `DELETE` on the companion tables. Use a different companion database for each realm
that has its own character database, and back up the two databases together.

Worldserver stops during startup if it cannot open or initialize the companion database.

## In-game commands

- `.companion help` shows the available commands and their arguments in game.
- `.companion add [name]` recruits an online bot in your group. Omit the name to use your selected target.
- `.companion list` shows this character’s recruits and whether each is in your group, online as a bot, or unavailable.
- `.companion info <name>` shows the companion's loyalty, relationship tier, and five most recent shared memories.
  It works even when the bot is offline.
- `.companion invite <name>` sends a standard party invitation to an online recruit. You need normal
  invitation permissions, and Playerbots decides whether the bot accepts.
- `.companion dismiss [name]` removes a recruited bot from your current party or raid. Omit the name to
  use your selected target. You need normal group removal permissions.
- `.companion remove <name or character ID>` erases the relationship and its journal, even if the bot is offline.
  It also clears the outing request and removes the bot from saved lineups. Use the displayed numeric ID
  if a listed character has become unavailable. The bot can stay in your party.

### Journal and traditions

- `.companion journal <name> [category] [page]` shows up to ten memories per page, newest first.
  Categories are `all`, `adventure`, `boss`, `dungeon`, `milestone`, `tradition`, and `pinned`.
  For example, `.companion journal BotName dungeon 2` shows the second page of dungeon completions.
- `.companion pin <name> <memory ID>` saves a memory to the `pinned` view. Use the ID displayed in the journal.
  `.companion unpin <name> <memory ID>` removes it from that view.
- `.companion tradition set <name> <nickname|joke|rendezvous> <text>` saves or replaces that kind of tradition
  for the pair. Use up to 60 characters of plain text. For example,
  `.companion tradition set BotName rendezvous The inn in Goldshire`.
- `.companion tradition list <name>` lists current traditions.
  `.companion tradition clear <name> <nickname|joke|rendezvous>` clears one.

### Saved lineups

- `.companion lineup save <lineup>` saves the online recruits in your current party or raid. Saving with
  an existing name replaces that lineup. Names are case-insensitive and allow 1–32 letters, digits,
  hyphens, or underscores.
- `.companion lineup list` lists your saved lineups. `.companion lineup show <lineup>` shows its recruits.
- `.companion lineup invite <lineup>` attempts normal invitations, up to the available slots.
  The command reports already-grouped, busy, and unavailable companions individually.
- `.companion lineup remove <lineup>` deletes the saved lineup.

### Outing requests

- `.companion request show <name>` shows the recruit's current suggestion or most recent outcome.
- `.companion request accept <name>` accepts the suggestion. Finish the named dungeon with the bot in
  your group to complete the outing.
- `.companion request decline <name>` declines a suggestion or cancels an accepted outing.

When a new party session earns loyalty, each companion has a 25% chance to look for an outing to suggest.
It uses a recent shared boss defeat or completion from a five-player dungeon with known completion credit.
A companion can have one outstanding request at a time, and offers are at least seven days apart. Outings
are optional and grant no extra loyalty, XP, or rewards. Either supported difficulty of the named dungeon
can fulfill an accepted outing.

## Loyalty

Each recruit starts at zero. Spending five minutes within 100 yards of a recruited bot while grouped
earns five loyalty, up to 100. Both characters must be in the same map and phase, and the player must
be connected and free of the AFK flag. Dungeon Finder groups qualify. Battleground and battlefield
groups are excluded.

A gap under two minutes pauses the clock and counts as part of the same session. At two minutes apart,
a future reunion starts a fresh session. Once a session qualifies, it earns five points total and keeps
the last shared activity time current while you stay together. If worldserver restarts before the session
qualifies, the five-minute timer starts again. Earned loyalty is persistent.

After seven inactive days, the relationship loses one loyalty point per additional full day, even
while you are offline. It stays on your roster at zero. The tiers are Newly recruited (0–19), Familiar
(20–49), Trusted (50–79), and Devoted (80–100). Tiers are visible in `.companion info` and are
descriptive in this release.

## Memories

- **Shared adventure.** Each party session that earns loyalty also records where you were together
  when it qualified. The journal keeps recording new adventures at the loyalty cap.
- **Boss defeat.** The journal records newly credited instance encounters when both characters are present
  in the same party or raid. It also records outdoor world bosses within the normal group reward range.
- **Dungeon completion.** The final encounter of a supported five-player dungeon records both the
  boss defeat and the dungeon completion. This works for manually formed and Dungeon Finder groups.
  Instance coverage follows AzerothCore's encounter data and completion credit.
- **Bond milestones.** Each relationship records its first shared dungeon, first time reaching Trusted
  and Devoted, and first victory after a shared wipe. A wipe counts when the encounter resets or fails
  with no living group members present, and the same pair later wins that encounter in the same instance.
  Wipe tracking starts when worldserver starts. Earned milestones survive loyalty decay and restarts.
- **Traditions.** Each pair can keep one nickname, one joke, and one rendezvous in its journal.

The journal starts recording with the first new adventure or encounter after the upgrade. Memories
stay as loyalty decays. Dismissing a companion keeps its journal. Removing the relationship also
deletes its memories.

Existing journals are preserved when upgrading. The next qualifying shared adventure or dungeon completion
uses the earliest saved completion to record the first-dungeon milestone. The next qualifying adventure also
records any loyalty milestones that the pair already meets.

### Read-only access

The `playerbot_companion_memories` table in the companion database contains structured event types,
map and instance IDs, difficulty, subject IDs, Unix timestamps, readable summaries, and a pin flag.
The subject ID is the zone for an adventure, the encounter ID for an instance boss, the creature entry
for an outdoor boss, the Dungeon Finder dungeon ID for a dungeon completion, one of `1` (first dungeon),
`2` (Trusted), `4` (Devoted), or `8` (wipe victory) for a milestone, or `1` (nickname), `2` (joke), or
`3` (rendezvous) for a tradition. An optional consumer such as the chatter bridge can query the journal
using a read-only MySQL account. Join to
`playerbot_companions` on all four character identity columns to read the current relationship.
Use its `loyalty` and `last_shared_at` fields with the decay rule above to calculate the current tier.

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
6. Check `.companion info BotName` for a new recruit. It should show zero. Stay grouped and close for
   five minutes, then check for five points. Dismiss and rejoin within two minutes to verify that it
   stays at five. Spend at least two minutes apart and qualify a new session to reach ten.
7. Restart worldserver and check `.companion info BotName` again. On a disposable test database,
   set that relationship's `last_shared_at` to eight days ago (Unix time in seconds) and confirm that
   the displayed score drops by one.
8. Qualify a party session and check for one adventure under `.companion info BotName`. A short
   disconnect or rejoin should leave that count unchanged.
9. Defeat a boss in a supported dungeon with the companion present. Finish the dungeon and verify the
   separate completion memory. Restart and verify that the journal still appears. Remove and
   re-recruit the companion to verify that the new relationship starts with an empty journal.
10. Dismiss an online companion and run `.companion invite BotName`. Verify that Playerbots handles the
    invitation. In a party with no open slots, verify that an additional companion stays outside the group.
11. Run `.companion journal BotName all 1`, pin a visible ID, and confirm it appears under `pinned`.
    Unpin it and verify that it still appears in `all`. With 11 or more memories, check page 2.
12. Set a nickname, joke, and rendezvous. Verify them under `.companion tradition list BotName` and the
    `tradition` filter. Replace a joke and clear the rendezvous, then check the list again.
13. Save a party as `.companion lineup save dungeon`, then list and show it. Regroup with
    `.companion lineup invite dungeon`. Remove a recruit and verify that the lineup loses that member.
14. Check `milestone` after your first supported dungeon completion. Wipe and defeat the same encounter
    in one instance and check for a new milestone. On a disposable test database, set loyalty just below
    50 or 80 before starting a fresh session. Verify that reaching each tier adds only one milestone.
15. After sharing a supported dungeon encounter, qualify new sessions until an outing is offered. Accept it,
    finish the named dungeon together, and check `.companion request show BotName` for completion. With a
    different recruit, decline an offer and verify that the status persists across a restart.

## Limitations

- The roster is per character. Random bots remain in Playerbots’ shared pool and can appear on multiple
  rosters. Bring offline recruits online through Playerbots before inviting them with a companion command.
- Companion invitations and dismissals apply to ordinary parties and raids. Dungeon Finder, battleground,
  and battlefield groups use their own rules. A bot played by a person, including SelfBot characters, is ineligible.
- Wipe victories depend on the instance script reporting the encounter reset and victory while both
  characters are present. Pending wipes are held in memory until victory, instance removal, or restart.
- A deleted or otherwise unavailable character can leave a roster entry behind. Use its displayed ID
  to clear it. Character creation times protect saved relationships if AzerothCore reuses a numeric ID.
