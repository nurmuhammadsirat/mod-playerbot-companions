#ifndef PLAYERBOT_COMPANIONS_SOCIAL_H
#define PLAYERBOT_COMPANIONS_SOCIAL_H

#include "AsyncCallbackProcessor.h"
#include "ChatCommand.h"
#include "CompanionDatabase.h"
#include "ObjectGuid.h"
#include "QueryCallback.h"
#include "Transaction.h"
#include <functional>
#include <string>

class ChatHandler;
class Player;

namespace Acore::PlayerbotCompanions
{
struct CharacterIdentity
{
    explicit CharacterIdentity(Player const& player);
    CharacterIdentity(ObjectGuid guid, int64 createdAt) : Guid(guid), CreatedAt(createdAt) { }

    Player* FindPlayer() const;
    bool operator<(CharacterIdentity const& other) const;
    bool operator==(CharacterIdentity const&) const = default;

    ObjectGuid Guid;
    int64 CreatedAt;
};

struct MemoryLocation
{
    uint32 MapId = 0;
    uint32 InstanceId = 0;
    uint8 Difficulty = 0;
    uint32 ZoneId = 0;
};

enum class BondMilestone : uint8
{
    FirstDungeon = 1,
    Trusted = 2,
    Devoted = 4,
    WipeVictory = 8
};

using CompanionTransaction = std::shared_ptr<Transaction<CompanionConnection>>;

extern AsyncCallbackProcessor<QueryCallback> QueryCallbacks;
extern AsyncCallbackProcessor<TransactionCallback> WriteCallbacks;

bool IsHuman(Player* player);
bool IsBot(Player* player);
Player* GetCommandPlayer(ChatHandler* handler);
bool WriteRelationship(CharacterIdentity owner, CompanionTransaction transaction, std::string message,
    std::function<void()> onSaved = {});
void AppendMilestone(CompanionTransaction const& transaction, CharacterIdentity const& owner,
    CharacterIdentity const& bot, BondMilestone milestone, uint8 minimumLoyalty, int64 occurredAt,
    MemoryLocation const& location, std::string_view summary, int64 recruitedAt = 0);
void AppendFirstDungeon(CompanionTransaction const& transaction, CharacterIdentity const& owner,
    CharacterIdentity const& bot);
void MaybeOfferOuting(CharacterIdentity owner, CharacterIdentity bot, int64 recruitedAt, int64 occurredAt);
void AddSocialCommands(ChatCommands::ChatCommandTable& commands);
}

#endif
