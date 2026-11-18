#include "AsyncCallbackProcessor.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ChatCommandTags.h"
#include "CommandScript.h"
#include "CompanionDatabase.h"
#include "Field.h"
#include "GameTime.h"
#include "Group.h"
#include "GroupReference.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PlayerbotOperations.h"
#include "QueryCallback.h"
#include "QueryResult.h"
#include "Transaction.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <functional>
#include <map>
#include <tuple>

namespace Acore::PlayerbotCompanions
{
namespace
{
using namespace ChatCommands;

// Chat commands and these callbacks both run on the world thread.
AsyncCallbackProcessor<QueryCallback> QueryCallbacks;
AsyncCallbackProcessor<TransactionCallback> WriteCallbacks;

struct CharacterIdentity
{
    explicit CharacterIdentity(Player const& player)
        : Guid(player.GetGUID()), CreatedAt(player.GetCreationTime().count()) { }

    Player* FindPlayer() const
    {
        Player* player = ObjectAccessor::FindPlayer(Guid);
        if (player && player->GetCreationTime().count() == CreatedAt)
            return player;

        return nullptr;
    }

    bool operator<(CharacterIdentity const& other) const
    {
        return std::tie(Guid, CreatedAt) < std::tie(other.Guid, other.CreatedAt);
    }

    bool operator==(CharacterIdentity const&) const = default;

    ObjectGuid Guid;
    int64 CreatedAt;
};

bool IsHuman(Player* player)
{
    return player && player->GetSession() && !player->GetSession()->IsHeadless() && IsRealPlayer(player);
}

bool IsBot(Player* player)
{
    return player && player->GetSession() && player->GetSession()->IsHeadless() &&
        sPlayerbotsMgr.GetPlayerbotAI(player);
}

TransactionCallback CommitStatement(CompanionPreparedStatement* statement)
{
    auto transaction = std::make_shared<Transaction<CompanionConnection>>();
    transaction->Append(statement);
    return CompanionDatabase.AsyncCommitTransaction(transaction);
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

                Observe({ CharacterIdentity(*owner), CharacterIdentity(*bot) }, now, previousSample, wallTime);
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
    };

    void Observe(Pair const& pair, TimePoint now, TimePoint previousSample, Seconds wallTime)
    {
        Session& session = _sessions.try_emplace(pair, now, wallTime, now, wallTime).first->second;
        // Only credit an interval with the pair present at both samples. Long server stalls add no time.
        if (session.LastSeen == previousSample && now - previousSample < 2 * SampleInterval)
            session.SharedTime += std::chrono::duration_cast<Milliseconds>(now - previousSample);

        session.LastSeen = now;
        session.LastSeenOn = wallTime;
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
        auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_UPD_LOYALTY);
        statement->SetArguments(lastSeen.count(), (session.StartedOn - RejoinGrace).count(), lastSeen.count(),
            owner.Guid.GetCounter(), owner.CreatedAt, bot.Guid.GetCounter(), bot.CreatedAt,
            lastSeen.count(), session.RecruitedOn);
        session.SavePending = true;
        WriteCallbacks.AddCallback(CommitStatement(statement))
            .AfterComplete([this, pair, startedAt = session.StartedAt, lastSeen](bool success)
            {
                auto const current = _sessions.find(pair);
                if (current == _sessions.end() || current->second.StartedAt != startedAt)
                    return;

                current->second.SavePending = false;
                if (success)
                    current->second.LastSavedOn = lastSeen;
            });
    }

    TimePoint _lastSample{};
    std::map<Pair, Session> _sessions;
};

PartyLoyaltyTracker PartyLoyalty;

Player* GetCommandPlayer(ChatHandler* handler)
{
    Player* player = handler->GetPlayer();
    if (IsHuman(player))
        return player;

    handler->SendErrorMessage("Companion commands require a manually controlled player character.");
    return nullptr;
}

bool WriteRelationship(CharacterIdentity owner, CompanionPreparedStatement* statement, std::string message,
    std::function<void()> onSaved)
{
    WriteCallbacks.AddCallback(CommitStatement(statement))
        .AfterComplete([owner, message = std::move(message), onSaved = std::move(onSaved)](bool success)
        {
            if (success)
                onSaved();

            Player* player = owner.FindPlayer();
            if (!IsHuman(player))
                return;

            ChatHandler reply(player->GetSession());
            if (success)
                reply.SendSysMessage(message);
            else
                reply.SendSysMessage("Could not save the companion change. Please try again.");
        });
    return true;
}

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
    auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_INS_RELATIONSHIP);
    statement->SetArguments(owner.Guid.GetCounter(), owner.CreatedAt, companion.Guid.GetCounter(),
        companion.CreatedAt, companion.CreatedAt, companion.CreatedAt, companion.CreatedAt, companion.CreatedAt);
    return WriteRelationship(owner, statement, Acore::StringFormat("{} is on your companion list.", bot->GetName()),
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
                reply.SendSysMessage("Could not load this companion's loyalty. Please try again.");
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
    if (target.holds_alternative<PlayerIdentifier>())
    {
        PlayerIdentifier const& companion = target.get<PlayerIdentifier>();
        auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_DEL_RELATIONSHIP);
        statement->SetArguments(owner.Guid.GetCounter(), owner.CreatedAt, companion.GetGUID().GetCounter());
        return WriteRelationship(owner, statement,
            Acore::StringFormat("{} is no longer on your companion list.", companion.GetName()),
            [owner, guid = companion.GetGUID()] { PartyLoyalty.OnRemove(owner, guid); });
    }

    auto const botGuid = target.get<ObjectGuid::LowType>();
    if (!botGuid)
    {
        handler->SendErrorMessage("Use a companion name or character ID from .companion list.");
        return false;
    }

    auto* statement = CompanionDatabase.GetPreparedStatement(COMPANION_DEL_RELATIONSHIP);
    statement->SetArguments(owner.Guid.GetCounter(), owner.CreatedAt, botGuid);
    return WriteRelationship(owner, statement,
        Acore::StringFormat("Character {} is no longer on your companion list.", botGuid),
        [owner, guid = ObjectGuid::Create<HighGuid::Player>(botGuid)] { PartyLoyalty.OnRemove(owner, guid); });
}

class CompanionCommandScript : public CommandScript
{
public:
    CompanionCommandScript() : CommandScript("CompanionCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable const companions = {
            { "add", HandleAdd, SEC_PLAYER, Console::No },
            { "list", HandleList, SEC_PLAYER, Console::No },
            { "info", HandleInfo, SEC_PLAYER, Console::No },
            { "dismiss", HandleDismiss, SEC_PLAYER, Console::No },
            { "remove", HandleRemove, SEC_PLAYER, Console::No }
        };
        return { { "companion", companions } };
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
    }
};
}
}

void Addmod_playerbot_companionsScripts()
{
    using namespace Acore::PlayerbotCompanions;

    AddCompanionDatabaseScript();
    new CompanionCommandScript();
    new CompanionWorldScript();
}
