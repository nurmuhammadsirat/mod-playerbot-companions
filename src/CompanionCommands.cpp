#include "AsyncCallbackProcessor.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ChatCommandTags.h"
#include "CommandScript.h"
#include "CompanionDatabase.h"
#include "CompanionSocial.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Field.h"
#include "GameTime.h"
#include "GlobalScript.h"
#include "Group.h"
#include "GroupReference.h"
#include "InstanceScript.h"
#include "Map.h"
#include "MapMgr.h"
#include "MapReference.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PlayerbotOperations.h"
#include "QueryCallback.h"
#include "QueryResult.h"
#include "Random.h"
#include "Timer.h"
#include "Transaction.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <tuple>

namespace Acore::PlayerbotCompanions
{
using namespace ChatCommands;

// Chat commands and these callbacks both run on the world thread.
AsyncCallbackProcessor<QueryCallback> QueryCallbacks;
AsyncCallbackProcessor<TransactionCallback> WriteCallbacks;

CharacterIdentity::CharacterIdentity(Player const& player)
    : Guid(player.GetGUID()), CreatedAt(player.GetCreationTime().count()) { }

Player* CharacterIdentity::FindPlayer() const
{
    Player* player = ObjectAccessor::FindPlayer(Guid);
    if (player && player->GetCreationTime().count() == CreatedAt)
        return player;

    return nullptr;
}

bool CharacterIdentity::operator<(CharacterIdentity const& other) const
{
    return std::tie(Guid, CreatedAt) < std::tie(other.Guid, other.CreatedAt);
}

bool IsHuman(Player* player)
{
    return player && player->GetSession() && !player->GetSession()->IsHeadless() && IsRealPlayer(player);
}

bool IsBot(Player* player)
{
    return player && player->GetSession() && player->GetSession()->IsHeadless() &&
        sPlayerbotsMgr.GetPlayerbotAI(player);
}

Player* GetCommandPlayer(ChatHandler* handler)
{
    Player* player = handler->GetPlayer();
    if (IsHuman(player))
        return player;

    handler->SendErrorMessage("Companion commands require a manually controlled player character.");
    return nullptr;
}

bool WriteRelationship(CharacterIdentity owner, CompanionTransaction transaction, std::string message,
    std::function<void()> onSaved)
{
    WriteCallbacks.AddCallback(CompanionDatabase.AsyncCommitTransaction(transaction))
        .AfterComplete([owner, message = std::move(message), onSaved = std::move(onSaved)](bool success)
        {
            if (success && onSaved)
                onSaved();

            Player* player = owner.FindPlayer();
            if (!IsHuman(player))
                return;

            ChatHandler reply(player->GetSession());
            if (!success)
                reply.SendSysMessage("Could not save the companion change. Please try again.");
            else if (!message.empty())
                reply.SendSysMessage(message);
        });
    return true;
}

namespace
{
MemoryLocation GetMemoryLocation(WorldObject const& source)
{
    Map const* map = source.GetMap();
    return { map->GetId(), map->GetInstanceId(), uint8(map->GetDifficulty()), source.GetZoneId() };
}

std::string DescribeAdventure(MemoryLocation const& location)
{
    char const* name = nullptr;
    if (!location.InstanceId)
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(location.ZoneId))
            name = zone->area_name[DEFAULT_LOCALE];

    if (!name || !*name)
        if (MapEntry const* map = sMapStore.LookupEntry(location.MapId))
            name = map->name[DEFAULT_LOCALE];

    return (name && *name) ? Acore::StringFormat("Adventured together in {}.", name) : "Adventured together.";
}

class PartyLoyaltyTracker
{
public:
    void Update()
    {
        TimePoint const now = GameTime::Now();
        TimePoint const previousSample = _lastSample;
        if (now - previousSample < SampleInterval)
            return;

        _lastSample = now;
        std::erase_if(_sessions, [now](auto const& entry) { return now - entry.second.LastSeen >= RejoinGrace; });

        Seconds const wallTime = GameTime::GetGameTime();
        for (auto const& [accountId, worldSession] : sWorldSessionMgr->GetAllSessions())
        {
            Player* owner = worldSession->GetPlayer();
            if (!IsHuman(owner) || !owner->IsInWorld() || worldSession->IsSocketClosed() ||
                owner->IsBeingTeleported() || owner->isAFK())
                continue;

            Group* group = owner->GetGroup();
            if (!group || group->isBGGroup() || group->isBFGroup())
                continue;

            for (GroupReference* member = group->GetFirstMember(); member; member = member->next())
            {
                Player* bot = member->GetSource();
                if (!IsBot(bot) || bot->GetGroup() != group || !bot->IsInWorld() ||
                    bot->IsBeingTeleported() || !owner->IsWithinDistInMap(bot, PartyDistance))
                    continue;

                Observe({ CharacterIdentity(*owner), CharacterIdentity(*bot) }, now, previousSample, wallTime,
                    *owner);
            }
        }

        for (auto& [pair, session] : _sessions)
        {
            if (session.Recruitment != RecruitmentState::Recruited || session.SharedTime < RequiredPartyTime ||
                session.SavePending || session.LastSeenOn <= session.LastSavedOn)
                continue;

            // Save when the pair first qualifies, periodically while together, and when they separate.
            if (session.LastSavedOn == 0s || session.LastSeenOn - session.LastSavedOn >= SaveInterval ||
                session.LastSeen != now)
                Save(pair, session);
        }
    }

    void OnRecruit(CharacterIdentity const& owner, CharacterIdentity const& bot)
    {
        // A new recruit starts earning from recruitment. Repeating add for an existing recruit preserves its session.
        auto const session = _sessions.find({ owner, bot });
        if (session != _sessions.end() && session->second.Recruitment != RecruitmentState::Recruited)
            _sessions.erase(session);
    }

    void OnRemove(CharacterIdentity const& owner, ObjectGuid botGuid)
    {
        std::erase_if(_sessions, [&owner, botGuid](auto const& entry)
        {
            return entry.first.first == owner && entry.first.second.Guid == botGuid;
        });
    }

private:
    static constexpr auto SampleInterval = 5s;
    static constexpr auto RejoinGrace = 2min;
    static constexpr auto RequiredPartyTime = 5min;
    static constexpr auto SaveInterval = 1min;
    static constexpr float PartyDistance = 100.0f;

    using Pair = std::pair<CharacterIdentity, CharacterIdentity>;

    enum class RecruitmentState { Unchecked, Checking, Recruited, Unrecruited };

    struct Session
    {
        TimePoint StartedAt;
        Seconds StartedOn;
        TimePoint LastSeen;
        Seconds LastSeenOn;
        Milliseconds SharedTime = 0ms;
        Seconds LastSavedOn = 0s;
        int64 RecruitedOn = 0;
        RecruitmentState Recruitment = RecruitmentState::Unchecked;
        bool SavePending = false;
        MemoryLocation LastLocation;
    };

    void Observe(Pair const& pair, TimePoint now, TimePoint previousSample, Seconds wallTime, Player const& ownerPlayer)
    {
        Session& session = _sessions.try_emplace(pair, now, wallTime, now, wallTime).first->second;
        // Only credit an interval with the pair present at both samples. Long server stalls add no time.
        if (session.LastSeen == previousSample && now - previousSample < 2 * SampleInterval)
            session.SharedTime += std::chrono::duration_cast<Milliseconds>(now - previousSample);

        session.LastSeen = now;
        session.LastSeenOn = wallTime;
        if (session.LastSavedOn == 0s && session.SharedTime >= RequiredPartyTime)
            session.LastLocation = GetMemoryLocation(ownerPlayer);
        if (session.Recruitment != RecruitmentState::Unchecked)
            return;

        session.Recruitment = RecruitmentState::Checking;
        auto const& [owner, bot] = pair;
        auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_SEL_RELATIONSHIP);
        statement->SetArguments(owner.Guid.GetCounter(), owner.CreatedAt, bot.Guid.GetCounter(), bot.CreatedAt);
        QueryCallbacks.AddCallback(CompanionDatabase.AsyncQuery(statement).WithPreparedCallback(
            [this, pair, startedAt = session.StartedAt](PreparedQueryResult result)
            {
                auto const current = _sessions.find(pair);
                if (current == _sessions.end() || current->second.StartedAt != startedAt)
                    return;

                Session& session = current->second;
                if (!result)
                {
                    session.Recruitment = RecruitmentState::Unchecked;
                    return;
                }

                Field const& recruitedOn = result->Fetch()[0];
                session.Recruitment = recruitedOn.IsNull() ?
                    RecruitmentState::Unrecruited : RecruitmentState::Recruited;
                session.RecruitedOn = recruitedOn.Get<int64>();
            }));
    }

    void Save(Pair const& pair, Session& session)
    {
        auto const& [owner, bot] = pair;
        Seconds const lastSeen = session.LastSeenOn;
        auto const sessionCutoff = (session.StartedOn - RejoinGrace).count();
        auto transaction = std::make_shared<Transaction<CompanionConnection>>();
        bool const firstSave = session.LastSavedOn == 0s;
        if (firstSave)
        {
            MemoryLocation const& location = session.LastLocation;
            auto* memory = CompanionDatabase.GetPreparedStatement(COMPANION_INS_ADVENTURE_MEMORY);
            memory->SetArguments(location.MapId, location.InstanceId, location.Difficulty, location.ZoneId,
                lastSeen.count(), DescribeAdventure(location), owner.Guid.GetCounter(), owner.CreatedAt,
                bot.Guid.GetCounter(), bot.CreatedAt, sessionCutoff, lastSeen.count(), session.RecruitedOn);
            transaction->Append(memory);
        }

        auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_UPD_LOYALTY);
        statement->SetArguments(lastSeen.count(), sessionCutoff, lastSeen.count(),
            owner.Guid.GetCounter(), owner.CreatedAt, bot.Guid.GetCounter(), bot.CreatedAt,
            lastSeen.count(), session.RecruitedOn);
        transaction->Append(statement);
        if (firstSave)
        {
            AppendFirstDungeon(transaction, owner, bot);
            AppendMilestone(transaction, owner, bot, BondMilestone::Trusted, 50, lastSeen.count(),
                session.LastLocation, "Our time adventuring together earned a trusted bond.", session.RecruitedOn);
            AppendMilestone(transaction, owner, bot, BondMilestone::Devoted, 80, lastSeen.count(),
                session.LastLocation, "Our shared adventures forged a devoted partnership.", session.RecruitedOn);
        }
        session.SavePending = true;
        bool const considerOuting = firstSave && roll_chance_i(25);
        WriteCallbacks.AddCallback(CompanionDatabase.AsyncCommitTransaction(transaction))
            .AfterComplete([this, pair, startedAt = session.StartedAt, lastSeen, considerOuting](bool success)
            {
                auto const current = _sessions.find(pair);
                if (current == _sessions.end() || current->second.StartedAt != startedAt)
                    return;

                current->second.SavePending = false;
                if (success)
                {
                    current->second.LastSavedOn = lastSeen;
                    if (considerOuting)
                        MaybeOfferOuting(pair.first, pair.second, current->second.RecruitedOn, lastSeen.count());
                }
            });
    }

    TimePoint _lastSample{};
    std::map<Pair, Session> _sessions;
};

PartyLoyaltyTracker PartyLoyalty;

Player* GetTargetBot(ChatHandler* handler, Player* owner, Optional<PlayerIdentifier> const& target)
{
    Optional<PlayerIdentifier> const selected = target ? target : PlayerIdentifier::FromTarget(handler);
    Player* bot = selected ? ObjectAccessor::FindPlayer(selected->GetGUID()) : nullptr;
    Group const* group = owner->GetGroup();
    if (IsBot(bot) && group && bot->GetGroup() == group)
        return bot;

    handler->SendErrorMessage("Select or name an online playerbot in your group.");
    return nullptr;
}

bool HandleAdd(ChatHandler* handler, Optional<PlayerIdentifier> target)
{
    Player* player = GetCommandPlayer(handler);
    if (!player)
        return false;

    Player* bot = GetTargetBot(handler, player, target);
    if (!bot)
        return false;

    CharacterIdentity const owner(*player);
    CharacterIdentity const companion(*bot);
    auto const createdAt = companion.CreatedAt;
    auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_INS_RELATIONSHIP);
    statement->SetArguments(owner.Guid.GetCounter(), owner.CreatedAt, companion.Guid.GetCounter(),
        createdAt, createdAt, createdAt, createdAt, createdAt, createdAt, createdAt, createdAt, createdAt, createdAt);
    auto transaction = std::make_shared<Transaction<CompanionConnection>>();
    transaction->Append(statement);
    return WriteRelationship(owner, transaction, Acore::StringFormat("{} is on your companion list.", bot->GetName()),
        [owner, companion] { PartyLoyalty.OnRecruit(owner, companion); });
}

bool HandleList(ChatHandler* handler)
{
    Player* player = GetCommandPlayer(handler);
    if (!player)
        return false;

    CharacterIdentity const owner(*player);
    auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_SEL_ROSTER);
    statement->SetArguments(owner.Guid.GetCounter(), owner.CreatedAt);
    QueryCallbacks.AddCallback(CompanionDatabase.AsyncQuery(statement).WithPreparedCallback(
        [owner](PreparedQueryResult result)
        {
            Player* player = owner.FindPlayer();
            if (!IsHuman(player))
                return;

            ChatHandler reply(player->GetSession());
            if (!result)
            {
                reply.SendSysMessage("Could not load your companions. Please try again.");
                return;
            }

            if (result->GetRowCount() == 1)
                reply.SendSysMessage("Your companion list is empty.");

            do
            {
                Field const* fields = result->Fetch();
                if (fields[0].IsNull())
                    continue;

                auto const lowGuid = fields[0].Get<ObjectGuid::LowType>();
                ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
                Player* bot = ObjectAccessor::FindPlayer(guid);
                std::string name;
                if ((bot && bot->GetCreationTime().count() != fields[1].Get<int64>()) ||
                    !sCharacterCache->GetCharacterNameByGuid(guid, name) || name.empty())
                {
                    reply.PSendSysMessage("Character {} (unavailable)", lowGuid);
                    continue;
                }

                if (!IsBot(bot))
                    reply.PSendSysMessage("{} (unavailable, character {})", name, lowGuid);
                else if (player->GetGroup() && bot->GetGroup() == player->GetGroup())
                    reply.PSendSysMessage("{} (in your group)", name);
                else
                    reply.PSendSysMessage("{} (online)", name);
            } while (result->NextRow());
        }));
    return true;
}

std::string_view LoyaltyTier(int64 loyalty)
{
    if (loyalty >= 80)
        return "Devoted";
    if (loyalty >= 50)
        return "Trusted";
    if (loyalty >= 20)
        return "Familiar";

    return "Newly recruited";
}

bool HandleInfo(ChatHandler* handler, PlayerIdentifier target)
{
    Player* player = GetCommandPlayer(handler);
    if (!player)
        return false;

    CharacterIdentity const owner(*player);
    ObjectGuid const botGuid = target.GetGUID();
    auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_SEL_INFO);
    statement->SetArguments(GameTime::GetGameTime().count(), owner.Guid.GetCounter(), owner.CreatedAt,
        botGuid.GetCounter());
    QueryCallbacks.AddCallback(CompanionDatabase.AsyncQuery(statement).WithPreparedCallback(
        [owner, botGuid, name = target.GetName()](PreparedQueryResult result)
        {
            Player* player = owner.FindPlayer();
            if (!IsHuman(player))
                return;

            ChatHandler reply(player->GetSession());
            if (!result)
            {
                reply.SendSysMessage("Could not load this companion's information. Please try again.");
                return;
            }

            Field const* fields = result->Fetch();
            Player* bot = ObjectAccessor::FindConnectedPlayer(botGuid);
            if (fields[0].IsNull() || (bot && bot->GetCreationTime().count() != fields[0].Get<int64>()))
            {
                reply.PSendSysMessage("{} is not on your companion list.", name);
                return;
            }

            auto const loyalty = fields[1].Get<int64>();
            reply.PSendSysMessage("{} has {} / 100 loyalty ({}).", name, loyalty, LoyaltyTier(loyalty));
            if (fields[2].IsNull())
            {
                reply.SendSysMessage("You have no shared memories yet.");
                return;
            }

            do
            {
                fields = result->Fetch();
                auto const date = Acore::Time::TimeToTimestampStr(Seconds(fields[2].Get<int64>()), "%Y-%m-%d");
                reply.PSendSysMessage("{}  {}", date, fields[3].Get<std::string_view>());
            } while (result->NextRow());
        }));
    return true;
}

bool CanDismiss(Player* owner, Player* bot)
{
    Group* group = owner->GetGroup();
    return group && bot->GetGroup() == group && !group->isLFGGroup() && !group->isBGGroup() && !group->isBFGroup() &&
        !group->IsLeader(bot->GetGUID()) && owner->CanUninviteFromGroup(bot->GetGUID()) == ERR_PARTY_RESULT_OK;
}

bool HandleDismiss(ChatHandler* handler, Optional<PlayerIdentifier> target)
{
    Player* player = GetCommandPlayer(handler);
    if (!player)
        return false;

    Player* bot = GetTargetBot(handler, player, target);
    if (!bot)
        return false;

    if (!CanDismiss(player, bot))
    {
        handler->SendErrorMessage("You need permission to remove this companion from your party or raid.");
        return false;
    }

    CharacterIdentity const owner(*player);
    CharacterIdentity const companion(*bot);
    ObjectGuid const groupGuid = player->GetGroup()->GetGUID();
    auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_SEL_RELATIONSHIP);
    statement->SetArguments(owner.Guid.GetCounter(), owner.CreatedAt, companion.Guid.GetCounter(), companion.CreatedAt);
    QueryCallbacks.AddCallback(CompanionDatabase.AsyncQuery(statement).WithPreparedCallback(
        [owner, companion, groupGuid](PreparedQueryResult result)
        {
            Player* player = owner.FindPlayer();
            if (!IsHuman(player))
                return;

            ChatHandler reply(player->GetSession());
            if (!result)
            {
                reply.SendSysMessage("Could not check your companion list. Please try again.");
                return;
            }

            if (result->Fetch()[0].IsNull())
            {
                reply.SendSysMessage("That playerbot has not been recruited by this character.");
                return;
            }

            Player* bot = companion.FindPlayer();
            if (!IsBot(bot) || !CanDismiss(player, bot) || player->GetGroup()->GetGUID() != groupGuid)
            {
                reply.SendSysMessage("The group changed before the companion could be dismissed.");
                return;
            }

            // Run the Playerbots operation while both characters are still in the checked group.
            std::string const name = bot->GetName();
            GroupRemoveMemberOperation remove(owner.Guid, companion.Guid);
            if (remove.Execute())
                reply.PSendSysMessage("{} left your group.", name);
            else
                reply.PSendSysMessage("Could not dismiss {} from your group.", name);
        }));
    return true;
}

bool HandleRemove(ChatHandler* handler, Variant<ObjectGuid::LowType, PlayerIdentifier> target)
{
    Player* player = GetCommandPlayer(handler);
    if (!player)
        return false;

    CharacterIdentity const owner(*player);
    bool const named = target.holds_alternative<PlayerIdentifier>();
    auto const botGuid = named ?
        target.get<PlayerIdentifier>().GetGUID().GetCounter() : target.get<ObjectGuid::LowType>();
    if (!botGuid)
    {
        handler->SendErrorMessage("Use a companion name or character ID from .companion list.");
        return false;
    }

    auto transaction = std::make_shared<Transaction<CompanionConnection>>();
    for (auto const type : { COMPANION_DEL_LINEUP_BOT, COMPANION_DEL_RELATIONSHIP })
    {
        auto* statement = CompanionDatabase.GetPreparedStatement(type);
        statement->SetArguments(owner.Guid.GetCounter(), owner.CreatedAt, botGuid);
        transaction->Append(statement);
    }
    std::string const name = named ? target.get<PlayerIdentifier>().GetName() :
        Acore::StringFormat("Character {}", botGuid);
    return WriteRelationship(owner, transaction, Acore::StringFormat("{} is no longer on your companion list.", name),
        [owner, guid = ObjectGuid::Create<HighGuid::Player>(botGuid)] { PartyLoyalty.OnRemove(owner, guid); });
}

bool HandleHelp(ChatHandler* handler)
{
    handler->SendSysMessage(
        "Companion roster\n"
        ".companion help — show this list\n"
        ".companion add [name] — recruit a bot in your group\n"
        ".companion list — show your recruits\n"
        ".companion info <name> — see loyalty and recent memories\n"
        ".companion invite <name> — invite an online recruit\n"
        ".companion dismiss [name] — remove a recruit from your party\n"
        ".companion remove <name or character ID> — erase the relationship\n"
        "For add and dismiss, omit the name to use your selected target.\n"
        "Journal and traditions\n"
        ".companion journal <name> [category] [page]\n"
        "Categories are all, adventure, boss, dungeon, milestone, tradition, and pinned.\n"
        ".companion pin <name> <memory ID>\n"
        ".companion unpin <name> <memory ID>\n"
        ".companion tradition list <name>\n"
        ".companion tradition set <name> <kind> <text>\n"
        ".companion tradition clear <name> <kind>\n"
        "Tradition kinds are nickname, joke, and rendezvous.\n"
        "Saved lineups\n"
        ".companion lineup list\n"
        ".companion lineup save <lineup>\n"
        ".companion lineup show <lineup>\n"
        ".companion lineup invite <lineup>\n"
        ".companion lineup remove <lineup>\n"
        "Dungeon outings\n"
        ".companion request show <name>\n"
        ".companion request accept <name>\n"
        ".companion request decline <name>");
    return true;
}

class CompanionCommandScript : public CommandScript
{
public:
    CompanionCommandScript() : CommandScript("CompanionCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable const companions = []
        {
            ChatCommandTable commands = {
                { "help", HandleHelp, SEC_PLAYER, Console::No },
                { "add", HandleAdd, SEC_PLAYER, Console::No },
                { "list", HandleList, SEC_PLAYER, Console::No },
                { "info", HandleInfo, SEC_PLAYER, Console::No },
                { "dismiss", HandleDismiss, SEC_PLAYER, Console::No },
                { "remove", HandleRemove, SEC_PLAYER, Console::No }
            };
            AddSocialCommands(commands);
            return commands;
        }();
        return { { "companion", companions } };
    }
};

struct EncounterMemory
{
    std::string_view Type;
    uint32 SubjectId;
    std::string Summary;
};

using CompanionPair = std::pair<CharacterIdentity, CharacterIdentity>;

std::vector<CompanionPair> GetPresentPairs(Map const& map, Group const* creditedGroup = nullptr,
    Unit const* source = nullptr, bool wiped = false)
{
    struct Participants
    {
        std::vector<CharacterIdentity> Owners;
        std::vector<CharacterIdentity> Bots;
        bool HasSurvivor = false;
    };

    std::map<Group const*, Participants> groups;
    for (auto const& member : map.GetPlayers())
    {
        Player* player = member.GetSource();
        if (!player || !player->IsInWorld() || player->IsSpectator() || player->IsBeingTeleported())
            continue;

        Group const* group = player->GetGroup();
        if (!group || group->isBGGroup() || group->isBFGroup() || (creditedGroup && group != creditedGroup))
            continue;

        if (source && !map.IsDungeon() &&
            (!player->InSamePhase(source) || !player->IsAtGroupRewardDistance(source)))
            continue;

        Participants& participants = groups[group];
        participants.HasSurvivor |= player->IsAlive();
        if (IsBot(player))
            participants.Bots.emplace_back(*player);
        else if (IsHuman(player) && !player->GetSession()->IsSocketClosed())
            participants.Owners.emplace_back(*player);
    }

    std::vector<CompanionPair> pairs;
    for (auto const& [group, participants] : groups)
    {
        if (wiped && participants.HasSurvivor)
            continue;

        for (CharacterIdentity const& owner : participants.Owners)
            for (CharacterIdentity const& bot : participants.Bots)
                pairs.emplace_back(owner, bot);
    }
    return pairs;
}

void RecordEncounter(Unit const& source, Group const* creditedGroup, EncounterMemory const& boss,
    Optional<EncounterMemory> const& completion = std::nullopt)
{
    auto transaction = std::make_shared<Transaction<CompanionConnection>>();
    MemoryLocation const location = GetMemoryLocation(source);
    auto const occurredAt = GameTime::GetGameTime().count();
    auto append = [&](CharacterIdentity const& owner, CharacterIdentity const& bot, EncounterMemory const& event)
    {
        auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_INS_ENCOUNTER_MEMORY);
        statement->SetArguments(event.Type, location.MapId, location.InstanceId, location.Difficulty,
            event.SubjectId, occurredAt, event.Summary, owner.Guid.GetCounter(), owner.CreatedAt,
            bot.Guid.GetCounter(), bot.CreatedAt);
        transaction->Append(statement);
    };

    for (auto const& [owner, bot] : GetPresentPairs(*source.GetMap(), creditedGroup, &source))
    {
        append(owner, bot, boss);
        if (!completion)
            continue;

        append(owner, bot, *completion);
        AppendFirstDungeon(transaction, owner, bot);
        auto* request = CompanionDatabase.GetPreparedStatement(COMPANION_UPD_COMPLETE_REQUEST);
        request->SetArguments(owner.Guid.GetCounter(), owner.CreatedAt, bot.Guid.GetCounter(),
            bot.CreatedAt, location.MapId);
        transaction->Append(request);
    }

    // Encounter hooks can run on map threads. Only the owned SQL transaction leaves this call.
    if (transaction->GetSize())
        CompanionDatabase.CommitTransaction(transaction);
}

class WipeVictoryTracker
{
public:
    void OnStateChanging(Map const& map, uint32 bossId, EncounterState oldState, EncounterState newState)
    {
        EncounterKey const key = { map.GetInstanceId(), map.GetId(), bossId };
        if (oldState == IN_PROGRESS && (newState == FAIL || newState == NOT_STARTED))
        {
            auto const pairs = GetPresentPairs(map, nullptr, nullptr, true);
            if (!pairs.empty())
            {
                std::lock_guard lock(_mutex);
                _wiped[key].insert(pairs.begin(), pairs.end());
            }
        }
        else if (newState == DONE && oldState != DONE && oldState != TO_BE_DECIDED)
        {
            std::lock_guard lock(_mutex);
            if (_wiped.contains(key))
                _victories.insert_or_assign(key, GetPresentPairs(map));
        }
    }

    void RemoveInstance(uint32 instanceId)
    {
        std::lock_guard lock(_mutex);
        std::erase_if(_wiped, [instanceId](auto const& entry) { return entry.first.InstanceId == instanceId; });
        std::erase_if(_victories, [instanceId](auto const& entry) { return entry.first.InstanceId == instanceId; });
    }

    void Update()
    {
        decltype(_victories) victories;
        {
            std::lock_guard lock(_mutex);
            if (_victories.empty())
                return;

            victories.swap(_victories);
        }

        auto transaction = std::make_shared<Transaction<CompanionConnection>>();
        for (auto const& [key, pairs] : victories)
        {
            Map const* map = sMapMgr->FindMap(key.MapId, key.InstanceId);
            InstanceMap const* instance = map ? map->ToInstanceMap() : nullptr;
            InstanceScript const* script = instance ? instance->GetInstanceScript() : nullptr;
            // Some scripts can reject DONE after the pre-state hook. Check the actual state after map updates.
            if (!script || script->GetBossState(key.BossId) != DONE)
                continue;

            std::set<CompanionPair> wiped;
            {
                std::lock_guard lock(_mutex);
                auto entry = _wiped.extract(key);
                if (entry.empty())
                    continue;
                wiped = std::move(entry.mapped());
            }

            MemoryLocation const location = { key.MapId, key.InstanceId, uint8(map->GetDifficulty()), 0 };
            auto const occurredAt = GameTime::GetGameTime().count();
            auto const summary = Acore::StringFormat(
                "We rallied after a wipe and defeated the encounter together in {}.", map->GetMapName());
            for (auto const& [owner, bot] : pairs)
                if (wiped.contains({ owner, bot }))
                    AppendMilestone(transaction, owner, bot, BondMilestone::WipeVictory, 0, occurredAt,
                        location, summary);
        }

        if (transaction->GetSize())
            CompanionDatabase.CommitTransaction(transaction);
    }

private:
    struct EncounterKey
    {
        uint32 InstanceId;
        uint32 MapId;
        uint32 BossId;

        auto operator<=>(EncounterKey const&) const = default;
    };

    std::mutex _mutex;
    std::map<EncounterKey, std::set<CompanionPair>> _wiped;
    std::map<EncounterKey, std::vector<CompanionPair>> _victories;
};

WipeVictoryTracker WipeVictories;

class CompanionEncounterScript : public GlobalScript
{
public:
    CompanionEncounterScript() : GlobalScript("CompanionEncounterScript", {
        GLOBALHOOK_ON_AFTER_UPDATE_ENCOUNTER_STATE,
        GLOBALHOOK_ON_BEFORE_SET_BOSS_STATE,
        GLOBALHOOK_ON_INSTANCEID_REMOVED
    }) { }

    void OnBeforeSetBossState(uint32 id, EncounterState newState, EncounterState oldState, Map* map) override
    {
        if (map && map->IsDungeon() && map->GetInstanceId())
            WipeVictories.OnStateChanging(*map, id, oldState, newState);
    }

    void OnInstanceIdRemoved(uint32 instanceId) override
    {
        WipeVictories.RemoveInstance(instanceId);
    }

    void OnAfterUpdateEncounterState(Map* map, EncounterCreditType type, uint32 creditEntry, Unit* source,
        Difficulty /*difficulty*/, DungeonEncounterList const* encounters, uint32 dungeonCompleted,
        bool updated) override
    {
        // The core sets updated when a credit changes the persisted encounter mask for this run.
        if (!updated || !map || !map->IsDungeon() || !source || !encounters)
            return;

        auto const matched = std::ranges::find_if(*encounters, [type, creditEntry, dungeonCompleted](auto const* entry)
        {
            return entry->creditType == type && entry->creditEntry == creditEntry &&
                (!dungeonCompleted || entry->lastEncounterDungeon == dungeonCompleted);
        });
        if (matched == encounters->end())
            return;

        Group const* creditedGroup = nullptr;
        if (type == ENCOUNTER_CREDIT_KILL_CREATURE)
        {
            if (Creature* creature = source->ToCreature())
                creditedGroup = creature->GetLootRecipientGroup();
        }
        else if (Player* caster = source->GetCharmerOrOwnerPlayerOrPlayerItself())
        {
            creditedGroup = caster->GetGroup();
            if (!creditedGroup)
                return;
        }

        DungeonEncounterEntry const& encounter = *(*matched)->dbcEntry;
        char const* name = encounter.encounterName[DEFAULT_LOCALE];
        if (!name || !*name)
            name = type == ENCOUNTER_CREDIT_KILL_CREATURE ? source->GetName().c_str() : "an encounter";

        char const* mapName = map->GetMapName();
        EncounterMemory const boss = {
            "boss_kill", encounter.id, Acore::StringFormat("Defeated {} together in {}.", name, mapName)
        };
        Optional<EncounterMemory> completion;
        if (dungeonCompleted && map->IsNonRaidDungeon())
            completion = EncounterMemory{
                "dungeon_completion", dungeonCompleted, Acore::StringFormat("Completed {} together.", mapName)
            };

        RecordEncounter(*source, creditedGroup, boss, completion);
    }
};

class CompanionWorldBossScript : public PlayerScript
{
public:
    CompanionWorldBossScript()
        : PlayerScript("CompanionWorldBossScript", { PLAYERHOOK_ON_CREATURE_KILL_CREDIT }) { }

    void OnPlayerCreatureKillCredit(Player* player, Creature* killed) override
    {
        Group const* group = player->GetGroup();
        if (!group || !killed->isWorldBoss() || !killed->GetMap()->IsWorldMap())
            return;

        RecordEncounter(*killed, group, {
            "boss_kill", killed->GetEntry(),
            Acore::StringFormat("Defeated {} together.", killed->GetName())
        });
    }
};

class CompanionWorldScript : public WorldScript
{
public:
    CompanionWorldScript() : WorldScript("CompanionWorldScript", { WORLDHOOK_ON_UPDATE }) { }

    void OnUpdate(uint32 /*diff*/) override
    {
        WriteCallbacks.ProcessReadyCallbacks();
        QueryCallbacks.ProcessReadyCallbacks();
        PartyLoyalty.Update();
        WipeVictories.Update();
    }
};
}
}

void Addmod_playerbot_companionsScripts()
{
    using namespace Acore::PlayerbotCompanions;

    AddCompanionDatabaseScript();
    new CompanionCommandScript();
    new CompanionEncounterScript();
    new CompanionWorldBossScript();
    new CompanionWorldScript();
}
