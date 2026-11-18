#include "AsyncCallbackProcessor.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ChatCommandTags.h"
#include "CommandScript.h"
#include "CompanionDatabase.h"
#include "Field.h"
#include "Group.h"
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

Player* GetCommandPlayer(ChatHandler* handler)
{
    Player* player = handler->GetPlayer();
    if (IsHuman(player))
        return player;

    handler->SendErrorMessage("Companion commands require a manually controlled player character.");
    return nullptr;
}

bool WriteRelationship(CharacterIdentity owner, CompanionPreparedStatement* statement, std::string message)
{
    auto transaction = std::make_shared<Transaction<CompanionConnection>>();
    transaction->Append(statement);

    WriteCallbacks.AddCallback(CompanionDatabase.AsyncCommitTransaction(transaction))
        .AfterComplete([owner, message = std::move(message)](bool success)
        {
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
        companion.CreatedAt, companion.CreatedAt, companion.CreatedAt);
    return WriteRelationship(owner, statement, Acore::StringFormat("{} is on your companion list.", bot->GetName()));
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

            if (!result->Fetch()[0].Get<int64>())
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
            Acore::StringFormat("{} is no longer on your companion list.", companion.GetName()));
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
        Acore::StringFormat("Character {} is no longer on your companion list.", botGuid));
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
