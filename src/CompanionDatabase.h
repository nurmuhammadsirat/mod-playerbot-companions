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
