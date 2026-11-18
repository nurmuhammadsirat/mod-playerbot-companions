#include "CompanionSocial.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ChatCommandTags.h"
#include "DBCStores.h"
#include "Field.h"
#include "GameTime.h"
#include "Group.h"
#include "GroupReference.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QueryResult.h"
#include "Timer.h"
#include "Util.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <vector>

namespace Acore::PlayerbotCompanions
{
namespace
{
using namespace ChatCommands;

constexpr uint32 JournalPageSize = 10;
constexpr Seconds OutingCooldown = 7_days;

enum class RequestState : uint8
{
    None,
    Offered,
    Accepted,
    Completed,
    Declined
};

struct Recruit
{
    CharacterIdentity Owner;
    CharacterIdentity Bot;
    int64 RecruitedAt;
    uint32 RequestMap;
    RequestState Status;
    bool RepeatOuting;
    int64 RequestAt;
};

void ReadForPlayer(CharacterIdentity owner, CompanionPreparedStatement* statement,
    std::function<void(Player&, PreparedQueryResult)> read)
{
    QueryCallbacks.AddCallback(CompanionDatabase.AsyncQuery(statement).WithPreparedCallback(
        [owner, read = std::move(read)](PreparedQueryResult result)
        {
            Player* player = owner.FindPlayer();
            if (IsHuman(player))
                read(*player, std::move(result));
        }));
}

void ReadRecruit(CharacterIdentity owner, ObjectGuid botGuid, std::function<void(Player&, Recruit const&)> read,
    bool reportErrors = true)
{
    auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_SEL_DETAILS);
    statement->SetArguments(owner.Guid.GetCounter(), owner.CreatedAt, botGuid.GetCounter());
    ReadForPlayer(owner, statement, [owner, botGuid, read = std::move(read), reportErrors](
        Player& player, PreparedQueryResult result)
    {
        if (!result)
        {
            if (reportErrors)
                ChatHandler(player.GetSession()).SendSysMessage("Could not read your companion roster.");
            return;
        }

        Field const* fields = result->Fetch();
        Player* connected = ObjectAccessor::FindConnectedPlayer(botGuid);
        if (fields[0].IsNull() || (connected && connected->GetCreationTime().count() != fields[1].Get<int64>()))
        {
            if (reportErrors)
                ChatHandler(player.GetSession()).SendSysMessage("That character is not on your companion roster.");
            return;
        }

        read(player, { owner, CharacterIdentity(botGuid, fields[1].Get<int64>()), fields[0].Get<int64>(),
            uint32(fields[2].Get<uint64>()), RequestState(fields[3].Get<uint64>()),
            fields[4].Get<uint64>() != 0, fields[5].Get<int64>() });
    });
}

bool WithRecruit(ChatHandler* handler, PlayerIdentifier const& target,
    std::function<void(Player&, Recruit const&)> read)
{
    Player* player = GetCommandPlayer(handler);
    if (!player)
        return false;

    ReadRecruit(CharacterIdentity(*player), target.GetGUID(), std::move(read));
    return true;
}

void SaveStatement(CharacterIdentity owner, CompanionPreparedStatement* statement, std::string message,
    std::function<void()> onSaved = {})
{
    auto transaction = std::make_shared<Transaction<CompanionConnection>>();
    transaction->Append(statement);
    WriteRelationship(owner, transaction, std::move(message), std::move(onSaved));
}

std::string CharacterName(ObjectGuid guid)
{
    std::string name;
    if (sCharacterCache->GetCharacterNameByGuid(guid, name) && !name.empty())
        return name;

    return Acore::StringFormat("Character {}", guid.GetCounter());
}

void Invite(Player& owner, CharacterIdentity const& companion)
{
    ChatHandler reply(owner.GetSession());
    Player* bot = companion.FindPlayer();
    std::string const name = CharacterName(companion.Guid);
    if (!IsBot(bot))
    {
        reply.PSendSysMessage("{} is unavailable as an online playerbot.", name);
        return;
    }

    Group* group = owner.GetGroup();
    if (group && bot->GetGroup() == group)
    {
        reply.PSendSysMessage("{} is already in your group.", name);
        return;
    }

    if (bot->GetGroup() || bot->GetGroupInvite() || bot->IsBeingTeleported())
    {
        reply.PSendSysMessage("{} is busy with another group, invitation, or transfer.", name);
        return;
    }

    if (!group)
        group = owner.GetGroupInvite();
    if (group)
    {
        if (group->isBGGroup() || group->isBFGroup() || group->isLFGGroup() ||
            (!group->IsLeader(owner.GetGUID()) && !group->IsAssistant(owner.GetGUID())))
        {
            reply.SendSysMessage("You need permission to invite members into an ordinary party or raid.");
            return;
        }

        uint32 const capacity = group->isRaidGroup() ? MAXRAIDSIZE : MAXGROUPSIZE;
        if (group->GetMembersCount() + group->GetInviteeCount() >= capacity)
        {
            reply.PSendSysMessage("Your group has no open invitation slot for {}.", name);
            return;
        }
    }

    // Use the same invitation and bot security flow as a client party invite.
    WorldPacket packet(CMSG_GROUP_INVITE);
    packet << bot->GetName() << uint32(0);
    owner.GetSession()->HandleGroupInviteOpcode(packet);
    if (bot->GetGroupInvite())
        reply.PSendSysMessage("Invited {}. Playerbots will handle the response.", name);
    else
        reply.PSendSysMessage("The party system declined the invitation for {}.", name);
}

bool HandleInvite(ChatHandler* handler, PlayerIdentifier target)
{
    return WithRecruit(handler, target, [](Player& player, Recruit const& recruit)
    {
        Invite(player, recruit.Bot);
    });
}

std::string_view JournalCategory(std::string_view type)
{
    if (type == "boss")
        return "boss_kill";
    if (type == "dungeon")
        return "dungeon_completion";
    return type;
}

bool HandleJournal(ChatHandler* handler, PlayerIdentifier target, Optional<std::string_view> category,
    Optional<uint32> page)
{
    std::string_view const filter = JournalCategory(category.value_or("all"));
    constexpr std::array filters = { "all", "adventure", "boss_kill", "dungeon_completion", "milestone",
        "tradition", "pinned" };
    uint32 const pageNumber = page.value_or(1);
    if (std::ranges::find(filters, filter) == filters.end() || !pageNumber)
    {
        handler->SendErrorMessage("Choose all, adventure, boss, dungeon, milestone, tradition, or pinned and page 1+.");
        return false;
    }

    return WithRecruit(handler, target, [filter = std::string(filter), pageNumber](Player&, Recruit const& recruit)
    {
        auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_SEL_JOURNAL);
        statement->SetArguments(recruit.Owner.Guid.GetCounter(), recruit.Owner.CreatedAt,
            recruit.Bot.Guid.GetCounter(), recruit.Bot.CreatedAt,
            filter, filter, filter, uint64(pageNumber - 1) * JournalPageSize);
        ReadForPlayer(recruit.Owner, statement, [bot = recruit.Bot.Guid, pageNumber](
            Player& player, PreparedQueryResult result)
        {
            ChatHandler reply(player.GetSession());
            if (!result)
            {
                reply.SendSysMessage("There are no memories on this journal page.");
                return;
            }

            reply.PSendSysMessage("{}'s journal, page {}.", CharacterName(bot), pageNumber);
            uint32 count = 0;
            do
            {
                Field const* fields = result->Fetch();
                auto const date = Acore::Time::TimeToTimestampStr(Seconds(fields[1].Get<int64>()), "%Y-%m-%d");
                reply.PSendSysMessage("#{}  {}  {}{}", fields[0].Get<uint64>(), date,
                    fields[4].Get<uint8>() ? "[Pinned] " : "", fields[3].Get<std::string_view>());
            } while (++count < JournalPageSize && result->NextRow());

            if (result->GetRowCount() > JournalPageSize)
                reply.PSendSysMessage("More memories are available on page {}.", uint64(pageNumber) + 1);
        });
    });
}

bool SetPin(ChatHandler* handler, PlayerIdentifier const& target, uint64 memoryId, bool pin)
{
    return WithRecruit(handler, target, [memoryId, pin](Player&, Recruit const& recruit)
    {
        auto* read = CompanionDatabase.GetPreparedStatement(COMPANION_SEL_MEMORY);
        read->SetArguments(recruit.Owner.Guid.GetCounter(), recruit.Owner.CreatedAt,
            recruit.Bot.Guid.GetCounter(), recruit.Bot.CreatedAt, memoryId);
        ReadForPlayer(recruit.Owner, read, [recruit, memoryId, pin](Player& player, PreparedQueryResult result)
        {
            if (!result)
            {
                ChatHandler(player.GetSession()).SendSysMessage("Choose a memory ID from this companion's journal.");
                return;
            }

            auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_UPD_PIN);
            statement->SetArguments(uint8(pin), recruit.Owner.Guid.GetCounter(), recruit.Owner.CreatedAt,
                recruit.Bot.Guid.GetCounter(), recruit.Bot.CreatedAt, memoryId);
            SaveStatement(recruit.Owner, statement, Acore::StringFormat("Memory #{} {}.", memoryId,
                pin ? "pinned" : "unpinned"));
        });
    });
}

bool HandlePin(ChatHandler* handler, PlayerIdentifier target, uint64 memoryId)
{
    return SetPin(handler, target, memoryId, true);
}

bool HandleUnpin(ChatHandler* handler, PlayerIdentifier target, uint64 memoryId)
{
    return SetPin(handler, target, memoryId, false);
}

struct TraditionType
{
    std::string_view Name;
    std::string_view Description;
    uint32 Id;
};

Optional<TraditionType> GetTradition(std::string_view name)
{
    constexpr std::array<TraditionType, 3> types = {{
        { "nickname", "One nickname between us is", 1 },
        { "joke", "Our standing joke is", 2 },
        { "rendezvous", "Our favorite place to regroup is", 3 }
    }};
    for (auto const& type : types)
        if (type.Name == name)
            return type;
    return std::nullopt;
}

bool SetTradition(ChatHandler* handler, PlayerIdentifier const& target, std::string_view kind, std::string text)
{
    auto const type = GetTradition(kind);
    if (!type)
    {
        handler->SendErrorMessage("Choose nickname, joke, or rendezvous.");
        return false;
    }

    std::wstring decoded;
    if (text.size() > 240 || (!text.empty() && (!Utf8toWStr(text, decoded) || decoded.size() > 60)) ||
        std::ranges::any_of(decoded, [](wchar_t ch) { return ch < 32 || (ch >= 127 && ch <= 159); }) ||
        text.find_first_of("|<>`") != std::string::npos)
    {
        handler->SendErrorMessage("Use up to 60 characters of plain text for a tradition.");
        return false;
    }

    return WithRecruit(handler, target, [type = *type, text = std::move(text)](Player&, Recruit const& recruit)
    {
        auto transaction = std::make_shared<Transaction<CompanionConnection>>();
        auto* remove = CompanionDatabase.GetPreparedStatement(COMPANION_DEL_TRADITION);
        remove->SetArguments(recruit.Owner.Guid.GetCounter(), recruit.Owner.CreatedAt,
            recruit.Bot.Guid.GetCounter(), recruit.Bot.CreatedAt, type.Id);
        transaction->Append(remove);
        if (!text.empty())
        {
            auto* insert = CompanionDatabase.GetPreparedStatement(COMPANION_INS_ENCOUNTER_MEMORY);
            insert->SetArguments("tradition", uint32(0), uint32(0), uint8(0), type.Id,
                GameTime::GetGameTime().count(), Acore::StringFormat("{} \"{}\".", type.Description, text),
                recruit.Owner.Guid.GetCounter(), recruit.Owner.CreatedAt,
                recruit.Bot.Guid.GetCounter(), recruit.Bot.CreatedAt);
            transaction->Append(insert);
        }

        WriteRelationship(recruit.Owner, transaction, text.empty() ? "Tradition cleared." : "Tradition saved.");
    });
}

bool HandleTraditionSet(ChatHandler* handler, PlayerIdentifier target, std::string_view kind, Tail text)
{
    std::string trimmed = Acore::String::Trim(std::string(text));
    if (trimmed.empty())
    {
        handler->SendErrorMessage("Enter the tradition text.");
        return false;
    }
    return SetTradition(handler, target, kind, std::move(trimmed));
}

bool HandleTraditionClear(ChatHandler* handler, PlayerIdentifier target, std::string_view kind)
{
    return SetTradition(handler, target, kind, {});
}

bool HandleTraditionList(ChatHandler* handler, PlayerIdentifier target)
{
    return WithRecruit(handler, target, [](Player&, Recruit const& recruit)
    {
        auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_SEL_TRADITIONS);
        statement->SetArguments(recruit.Owner.Guid.GetCounter(), recruit.Owner.CreatedAt,
            recruit.Bot.Guid.GetCounter(), recruit.Bot.CreatedAt);
        ReadForPlayer(recruit.Owner, statement, [](Player& player, PreparedQueryResult result)
        {
            ChatHandler reply(player.GetSession());
            if (!result)
            {
                reply.SendSysMessage("You have no traditions with this companion yet.");
                return;
            }

            do
            {
                reply.SendSysMessage(result->Fetch()[0].Get<std::string_view>());
            } while (result->NextRow());
        });
    });
}

bool ValidLineupName(ChatHandler* handler, std::string_view name)
{
    if (!name.empty() && name.size() <= 32 && name.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string_view::npos)
        return true;

    handler->SendErrorMessage("Lineup names use 1 to 32 letters, digits, hyphens, or underscores.");
    return false;
}

bool HandleLineupSave(ChatHandler* handler, std::string_view name)
{
    Player* player = GetCommandPlayer(handler);
    if (!player || !ValidLineupName(handler, name))
        return false;

    Group* group = player->GetGroup();
    if (!group || group->isBGGroup() || group->isBFGroup())
    {
        handler->SendErrorMessage("Join your companions in a party before saving a lineup.");
        return false;
    }

    CharacterIdentity const owner(*player);
    ObjectGuid const groupGuid = group->GetGUID();
    auto* roster = CompanionDatabase.GetPreparedStatement(COMPANION_SEL_ROSTER);
    roster->SetArguments(owner.Guid.GetCounter(), owner.CreatedAt);
    ReadForPlayer(owner, roster, [owner, groupGuid, name = std::string(name)](
        Player& player, PreparedQueryResult result)
    {
        ChatHandler reply(player.GetSession());
        Group* current = player.GetGroup();
        if (!result || !current || current->GetGUID() != groupGuid)
        {
            reply.SendSysMessage("Could not read the lineup from your current party.");
            return;
        }

        auto transaction = std::make_shared<Transaction<CompanionConnection>>();
        auto* remove = CompanionDatabase.GetPreparedStatement(COMPANION_DEL_LINEUP);
        remove->SetArguments(owner.Guid.GetCounter(), owner.CreatedAt, name);
        transaction->Append(remove);
        do
        {
            Field const* fields = result->Fetch();
            if (fields[0].IsNull())
                continue;

            auto const lowGuid = fields[0].Get<ObjectGuid::LowType>();
            CharacterIdentity const companion(ObjectGuid::Create<HighGuid::Player>(lowGuid), fields[1].Get<int64>());
            Player* bot = companion.FindPlayer();
            if (!IsBot(bot) || bot->GetGroup() != current)
                continue;

            auto* insert = CompanionDatabase.GetPreparedStatement(COMPANION_INS_LINEUP_MEMBER);
            insert->SetArguments(name, owner.Guid.GetCounter(), owner.CreatedAt, lowGuid, companion.CreatedAt);
            transaction->Append(insert);
        } while (result->NextRow());

        auto const count = transaction->GetSize() - 1;
        if (!count)
        {
            reply.SendSysMessage("Your current party has no online recruits to save.");
            return;
        }
        WriteRelationship(owner, transaction,
            Acore::StringFormat("Saved lineup {} with {} companion(s).", name, count));
    });
    return true;
}

bool HandleLineupList(ChatHandler* handler)
{
    Player* player = GetCommandPlayer(handler);
    if (!player)
        return false;

    CharacterIdentity const owner(*player);
    auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_SEL_LINEUPS);
    statement->SetArguments(owner.Guid.GetCounter(), owner.CreatedAt);
    ReadForPlayer(owner, statement, [](Player& player, PreparedQueryResult result)
    {
        ChatHandler reply(player.GetSession());
        if (!result)
        {
            reply.SendSysMessage("You have no saved lineups.");
            return;
        }
        do
        {
            Field const* fields = result->Fetch();
            reply.PSendSysMessage("{} ({} companion(s))", fields[0].Get<std::string_view>(), fields[1].Get<uint64>());
        } while (result->NextRow());
    });
    return true;
}

bool ReadLineup(ChatHandler* handler, std::string_view name, bool sendInvitations)
{
    Player* player = GetCommandPlayer(handler);
    if (!player || !ValidLineupName(handler, name))
        return false;

    CharacterIdentity const owner(*player);
    auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_SEL_LINEUP);
    statement->SetArguments(owner.Guid.GetCounter(), owner.CreatedAt, name);
    ReadForPlayer(owner, statement, [sendInvitations](Player& player, PreparedQueryResult result)
    {
        ChatHandler reply(player.GetSession());
        if (!result)
        {
            reply.SendSysMessage("That lineup has no current recruits.");
            return;
        }

        do
        {
            Field const* fields = result->Fetch();
            CharacterIdentity const bot(ObjectGuid::Create<HighGuid::Player>(fields[0].Get<ObjectGuid::LowType>()),
                fields[1].Get<int64>());
            if (sendInvitations)
                Invite(player, bot);
            else
                reply.SendSysMessage(CharacterName(bot.Guid));
        } while (result->NextRow());
    });
    return true;
}

bool HandleLineupShow(ChatHandler* handler, std::string_view name)
{
    return ReadLineup(handler, name, false);
}

bool HandleLineupInvite(ChatHandler* handler, std::string_view name)
{
    return ReadLineup(handler, name, true);
}

bool HandleLineupRemove(ChatHandler* handler, std::string_view name)
{
    Player* player = GetCommandPlayer(handler);
    if (!player || !ValidLineupName(handler, name))
        return false;

    CharacterIdentity const owner(*player);
    auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_DEL_LINEUP);
    statement->SetArguments(owner.Guid.GetCounter(), owner.CreatedAt, name);
    SaveStatement(owner, statement, Acore::StringFormat("Lineup {} is no longer saved.", name));
    return true;
}

std::string DungeonName(uint32 id)
{
    if (MapEntry const* entry = sMapStore.LookupEntry(id))
        if (char const* name = entry->name[DEFAULT_LOCALE]; name && *name)
            return name;

    return Acore::StringFormat("dungeon {}", id);
}

void ShowOuting(Player& player, Recruit const& recruit)
{
    ChatHandler reply(player.GetSession());
    auto const name = CharacterName(recruit.Bot.Guid);
    auto const map = DungeonName(recruit.RequestMap);
    switch (recruit.Status)
    {
        case RequestState::Offered:
            reply.PSendSysMessage("{} suggests {} {}. Use .companion request accept {} if you'd like to go.",
                name, recruit.RepeatOuting ? "returning to" : "finishing", map, name);
            break;
        case RequestState::Accepted:
            reply.PSendSysMessage("Outing with {} accepted. Complete {} together.", name, map);
            break;
        case RequestState::Completed:
            reply.PSendSysMessage("You and {} completed your outing in {}.", name, map);
            break;
        case RequestState::Declined:
            reply.PSendSysMessage("You declined the outing with {}.", name);
            break;
        default:
            reply.PSendSysMessage("{} has no outing suggestion right now.", name);
            break;
    }
}

bool HandleOutingShow(ChatHandler* handler, PlayerIdentifier target)
{
    return WithRecruit(handler, target, ShowOuting);
}

bool SetOuting(ChatHandler* handler, PlayerIdentifier const& target, RequestState next)
{
    return WithRecruit(handler, target, [next](Player& player, Recruit const& recruit)
    {
        if (recruit.Status != RequestState::Offered &&
            !(next == RequestState::Declined && recruit.Status == RequestState::Accepted))
        {
            ShowOuting(player, recruit);
            return;
        }

        auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_UPD_REQUEST);
        statement->SetArguments(next, recruit.Owner.Guid.GetCounter(), recruit.Owner.CreatedAt,
            recruit.Bot.Guid.GetCounter(), recruit.Bot.CreatedAt, recruit.RecruitedAt,
            recruit.RequestAt, recruit.Status);
        SaveStatement(recruit.Owner, statement, "", [owner = recruit.Owner, bot = recruit.Bot.Guid]
        {
            ReadRecruit(owner, bot, ShowOuting);
        });
    });
}

bool HandleOutingAccept(ChatHandler* handler, PlayerIdentifier target)
{
    return SetOuting(handler, target, RequestState::Accepted);
}

bool HandleOutingDecline(ChatHandler* handler, PlayerIdentifier target)
{
    return SetOuting(handler, target, RequestState::Declined);
}
}

void AppendMilestone(CompanionTransaction const& transaction, CharacterIdentity const& owner,
    CharacterIdentity const& bot, BondMilestone milestone, uint8 minimumLoyalty, int64 occurredAt,
    MemoryLocation const& location, std::string_view summary, int64 recruitedAt)
{
    auto* memory = CompanionDatabase.GetPreparedStatement(COMPANION_INS_MILESTONE);
    memory->SetArguments(location.MapId, location.InstanceId, location.Difficulty, uint32(milestone),
        occurredAt, summary, owner.Guid.GetCounter(), owner.CreatedAt, bot.Guid.GetCounter(), bot.CreatedAt,
        milestone, minimumLoyalty, recruitedAt, recruitedAt);
    transaction->Append(memory);

    auto* flags = CompanionDatabase.GetPreparedStatement(COMPANION_UPD_MILESTONE);
    flags->SetArguments(milestone, owner.Guid.GetCounter(), owner.CreatedAt, bot.Guid.GetCounter(), bot.CreatedAt,
        minimumLoyalty, recruitedAt, recruitedAt);
    transaction->Append(flags);
}

void AppendFirstDungeon(CompanionTransaction const& transaction, CharacterIdentity const& owner,
    CharacterIdentity const& bot)
{
    constexpr auto milestone = BondMilestone::FirstDungeon;
    auto* memory = CompanionDatabase.GetPreparedStatement(COMPANION_INS_FIRST_DUNGEON);
    memory->SetArguments(uint32(milestone), owner.Guid.GetCounter(), owner.CreatedAt,
        bot.Guid.GetCounter(), bot.CreatedAt, milestone);
    transaction->Append(memory);

    auto* flag = CompanionDatabase.GetPreparedStatement(COMPANION_UPD_FIRST_DUNGEON);
    flag->SetArguments(milestone, owner.Guid.GetCounter(), owner.CreatedAt, bot.Guid.GetCounter(), bot.CreatedAt,
        milestone, uint32(milestone));
    transaction->Append(flag);
}

void MaybeOfferOuting(CharacterIdentity owner, CharacterIdentity bot, int64 recruitedAt, int64 occurredAt)
{
    auto const cutoff = occurredAt - OutingCooldown.count();
    auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_SEL_OUTING_CANDIDATES);
    statement->SetArguments(owner.Guid.GetCounter(), owner.CreatedAt, bot.Guid.GetCounter(), bot.CreatedAt,
        recruitedAt, cutoff, occurredAt);
    ReadForPlayer(owner, statement, [owner, bot, recruitedAt, occurredAt, cutoff](
        Player&, PreparedQueryResult result)
    {
        if (!result)
            return;

        do
        {
            Field const* fields = result->Fetch();
            auto const mapId = fields[0].Get<uint32>();
            MapEntry const* map = sMapStore.LookupEntry(mapId);
            if (!map || !map->IsNonRaidDungeon())
                continue;

            auto const difficulty = Difficulty(fields[1].Get<uint8>());
            auto const* encounters = sObjectMgr->GetDungeonEncounterList(mapId, difficulty);
            if (!encounters || std::ranges::none_of(*encounters, [](auto const* entry)
                { return entry->lastEncounterDungeon != 0; }))
                continue;

            auto* offer = CompanionDatabase.GetPreparedStatement(COMPANION_UPD_OFFER);
            bool const repeat = fields[2].Get<std::string_view>() == "dungeon_completion";
            offer->SetArguments(mapId, uint8(repeat), occurredAt, owner.Guid.GetCounter(), owner.CreatedAt,
                bot.Guid.GetCounter(), bot.CreatedAt, recruitedAt, cutoff);
            SaveStatement(owner, offer, "", [owner, bot, occurredAt]
            {
                ReadRecruit(owner, bot.Guid, [occurredAt](Player& player, Recruit const& recruit)
                {
                    if (recruit.Status == RequestState::Offered && recruit.RequestAt == occurredAt)
                        ShowOuting(player, recruit);
                }, false);
            });
            return;
        } while (result->NextRow());
    });
}

void AddSocialCommands(ChatCommandTable& commands)
{
    static ChatCommandTable const lineups = {
        { "save", HandleLineupSave, SEC_PLAYER, Console::No },
        { "list", HandleLineupList, SEC_PLAYER, Console::No },
        { "show", HandleLineupShow, SEC_PLAYER, Console::No },
        { "invite", HandleLineupInvite, SEC_PLAYER, Console::No },
        { "remove", HandleLineupRemove, SEC_PLAYER, Console::No }
    };
    static ChatCommandTable const traditions = {
        { "set", HandleTraditionSet, SEC_PLAYER, Console::No },
        { "clear", HandleTraditionClear, SEC_PLAYER, Console::No },
        { "list", HandleTraditionList, SEC_PLAYER, Console::No }
    };
    static ChatCommandTable const requests = {
        { "show", HandleOutingShow, SEC_PLAYER, Console::No },
        { "accept", HandleOutingAccept, SEC_PLAYER, Console::No },
        { "decline", HandleOutingDecline, SEC_PLAYER, Console::No }
    };
    commands.emplace_back("invite", HandleInvite, SEC_PLAYER, Console::No);
    commands.emplace_back("journal", HandleJournal, SEC_PLAYER, Console::No);
    commands.emplace_back("pin", HandlePin, SEC_PLAYER, Console::No);
    commands.emplace_back("unpin", HandleUnpin, SEC_PLAYER, Console::No);
    commands.emplace_back("lineup", lineups);
    commands.emplace_back("tradition", traditions);
    commands.emplace_back("request", requests);
}
}
