#ifndef PLAYERBOT_COMPANIONS_DATABASE_H
#define PLAYERBOT_COMPANIONS_DATABASE_H

#include "ModuleDatabasePool.h"

namespace Acore::PlayerbotCompanions
{
enum CompanionStatement : uint32
{
    COMPANION_INS_RELATIONSHIP,
    COMPANION_SEL_ROSTER,
    COMPANION_SEL_RELATIONSHIP,
    COMPANION_DEL_RELATIONSHIP,
    COMPANION_SEL_INFO,
    COMPANION_UPD_LOYALTY,
    COMPANION_INS_ADVENTURE_MEMORY,
    COMPANION_INS_ENCOUNTER_MEMORY,
    COMPANION_SEL_DETAILS,
    COMPANION_INS_MILESTONE,
    COMPANION_UPD_MILESTONE,
    COMPANION_INS_FIRST_DUNGEON,
    COMPANION_UPD_FIRST_DUNGEON,
    COMPANION_SEL_JOURNAL,
    COMPANION_SEL_MEMORY,
    COMPANION_UPD_PIN,
    COMPANION_SEL_TRADITIONS,
    COMPANION_DEL_TRADITION,
    COMPANION_DEL_LINEUP,
    COMPANION_DEL_LINEUP_BOT,
    COMPANION_INS_LINEUP_MEMBER,
    COMPANION_SEL_LINEUPS,
    COMPANION_SEL_LINEUP,
    COMPANION_SEL_OUTING_CANDIDATES,
    COMPANION_UPD_OFFER,
    COMPANION_UPD_REQUEST,
    COMPANION_UPD_COMPLETE_REQUEST,
    MAX_COMPANION_STATEMENTS
};

class CompanionConnection;
using CompanionPreparedStatement = PreparedStatement<CompanionConnection>;

class CompanionDatabasePool : public ModuleDatabasePool
{
public:
    CompanionPreparedStatement* GetPreparedStatement(CompanionStatement statement);

protected:
    MySQLConnection* CreateConnection(MySQLConnectionInfo& info) override;
    MySQLConnection* CreateAsyncConnection(ProducerConsumerQueue<SQLOperation*>* queue,
        MySQLConnectionInfo& info) override;
};

extern CompanionDatabasePool CompanionDatabase;

void AddCompanionDatabaseScript();
}

#endif
