#include "CompanionDatabase.h"
#include "BuiltInConfig.h"
#include "Config.h"
#include "DBUpdater.h"
#include "DatabaseEnv.h"
#include "DatabaseScript.h"
#include "Duration.h"
#include "Log.h"
#include "MySQLPreparedStatement.h"
#include "Util.h"
#include <mysqld_error.h>

namespace Acore::PlayerbotCompanions
{
namespace
{
constexpr int MaxLoyalty = 100;
constexpr int SessionLoyalty = 5;
constexpr int GraceDays = 7;

std::string DecayedLoyalty(std::string_view now)
{
    return Acore::StringFormat("GREATEST(0, CAST(loyalty AS SIGNED) - GREATEST(0, "
        "({} - last_shared_at) DIV {} - {}))", now, Seconds(24h).count(), GraceDays);
}
}

class CompanionConnection : public MySQLConnection
{
public:
    using MySQLConnection::MySQLConnection;

private:
    void DoPrepareStatements() override
    {
        if (!m_reconnecting)
            m_stmts.resize(MAX_COMPANION_STATEMENTS);

        PrepareStatement(COMPANION_INS_RELATIONSHIP,
            "INSERT INTO playerbot_companions (owner_guid, owner_created_at, bot_guid, bot_created_at) "
            "VALUES (?, ?, ?, ?) ON DUPLICATE KEY UPDATE "
            "loyalty = IF(bot_created_at = ?, loyalty, 0), last_shared_at = IF(bot_created_at = ?, last_shared_at, 0), "
            "recruited_at = IF(bot_created_at = ?, recruited_at, CURRENT_TIMESTAMP), bot_created_at = ?",
            CONNECTION_ASYNC);
        // A NULL row distinguishes an empty roster from a failed query.
        PrepareStatement(COMPANION_SEL_ROSTER,
            "SELECT NULL AS bot_guid, NULL AS bot_created_at UNION ALL "
            "SELECT bot_guid, bot_created_at FROM playerbot_companions "
            "WHERE owner_guid = ? AND owner_created_at = ? ORDER BY bot_guid", CONNECTION_ASYNC);
        PrepareStatement(COMPANION_SEL_RELATIONSHIP,
            "SELECT CAST(MAX(UNIX_TIMESTAMP(recruited_at)) AS SIGNED) FROM playerbot_companions "
            "WHERE owner_guid = ? AND owner_created_at = ? AND bot_guid = ? AND bot_created_at = ?",
            CONNECTION_ASYNC);
        PrepareStatement(COMPANION_DEL_RELATIONSHIP,
            "DELETE FROM playerbot_companions WHERE owner_guid = ? AND owner_created_at = ? AND bot_guid = ?",
            CONNECTION_ASYNC);
        PrepareStatement(COMPANION_SEL_INFO,
            Acore::StringFormat("SELECT MAX(bot_created_at), CAST(COALESCE(MAX({}), 0) AS SIGNED) "
                "FROM playerbot_companions WHERE owner_guid = ? AND owner_created_at = ? AND bot_guid = ?",
                DecayedLoyalty("?")), CONNECTION_ASYNC);
        // Once last_shared_at advances past the session cutoff, later saves and retries can only refresh it.
        PrepareStatement(COMPANION_UPD_LOYALTY,
            Acore::StringFormat("UPDATE playerbot_companions "
                "SET loyalty = LEAST({}, {} + IF(last_shared_at <= ?, {}, 0)), last_shared_at = ? "
                "WHERE owner_guid = ? AND owner_created_at = ? AND bot_guid = ? AND bot_created_at = ? "
                "AND last_shared_at < ? AND UNIX_TIMESTAMP(recruited_at) = ?",
                MaxLoyalty, DecayedLoyalty("?"), SessionLoyalty), CONNECTION_ASYNC);
    }
};

CompanionDatabasePool CompanionDatabase;

CompanionPreparedStatement* CompanionDatabasePool::GetPreparedStatement(CompanionStatement statement)
{
    return new CompanionPreparedStatement(statement, GetPreparedStatementParamCount(statement));
}

MySQLConnection* CompanionDatabasePool::CreateConnection(MySQLConnectionInfo& info)
{
    return new CompanionConnection(info);
}

MySQLConnection* CompanionDatabasePool::CreateAsyncConnection(ProducerConsumerQueue<SQLOperation*>* queue,
    MySQLConnectionInfo& info)
{
    return new CompanionConnection(queue, info);
}

namespace
{
std::string GetConnectionString()
{
    std::string info = sConfigMgr->GetOption<std::string>("PlayerbotCompanions.DatabaseInfo", "", false);
    if (!info.empty())
        return info;

    MySQLConnectionInfo const& characters = *CharacterDatabase.GetConnectionInfo();
    info = Acore::StringFormat("{};{};{};{};acore_playerbot_companions",
        characters.host, characters.port_or_socket, characters.user, characters.password);
    if (!characters.ssl.empty())
        info += ';' + characters.ssl;

    return info;
}

class CompanionDatabaseScript : public DatabaseScript
{
public:
    CompanionDatabaseScript() : DatabaseScript("CompanionDatabaseScript", {
        DATABASEHOOK_ON_MODULE_DATABASES_LOADING,
        DATABASEHOOK_ON_MODULE_DATABASES_KEEPALIVE,
        DATABASEHOOK_ON_MODULE_DATABASES_CLOSING
    }) { }

    bool OnModuleDatabasesLoading() override
    {
        std::string const connection = GetConnectionString();
        MySQLConnectionInfo const info(connection);
        MySQLConnectionInfo const playerbots(sConfigMgr->GetOption<std::string>("PlayerbotsDatabaseInfo", "", false));
        if (info.database.empty() ||
            info.database.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
                std::string::npos ||
            StringEqualI(info.database, LoginDatabase.GetConnectionInfo()->database) ||
            StringEqualI(info.database, CharacterDatabase.GetConnectionInfo()->database) ||
            StringEqualI(info.database, WorldDatabase.GetConnectionInfo()->database) ||
            StringEqualI(info.database, playerbots.database))
        {
            LOG_ERROR("server.loading", "PlayerbotCompanions.DatabaseInfo must name a dedicated companion database.");
            return false;
        }

        // A single worker processes roster reads and writes in the order players issue them.
        CompanionDatabase.SetConnectionInfo(connection, 1, 1);
        bool const updatesEnabled = sConfigMgr->GetOption<bool>("PlayerbotCompanions.DatabaseUpdates", true, false);
        uint32 error = CompanionDatabase.Open();
        if (error == ER_BAD_DB_ERROR && updatesEnabled && sConfigMgr->GetOption<bool>("Updates.AutoSetup", true))
        {
            if (!ModuleDBUpdater::Create(CompanionDatabase))
                return false;

            error = CompanionDatabase.Open();
        }

        if (error)
        {
            LOG_ERROR("server.loading", "Cannot open the companion database '{}', error {}.", info.database, error);
            return false;
        }

        if (updatesEnabled)
        {
            std::string const source = BuiltInConfig::GetSourceDirectory() + "/modules/mod-playerbot-companions";
            DBUpdaterInfo const updater = {
                "Playerbot Companions", source, source + "/data/sql/playerbot_companions/base", "playerbot_companions"
            };
            if (!ModuleDBUpdater::Populate(CompanionDatabase, updater) ||
                !ModuleDBUpdater::Update(CompanionDatabase, updater))
                return false;
        }

        if (!CompanionDatabase.PrepareStatements())
        {
            LOG_ERROR("server.loading", "Could not prepare companion statements. Check the companion SQL migration.");
            return false;
        }

        return true;
    }

    void OnModuleDatabasesKeepAlive() override { CompanionDatabase.KeepAlive(); }
    void OnModuleDatabasesClosing() override { CompanionDatabase.Close(); }
};
}

void AddCompanionDatabaseScript()
{
    new CompanionDatabaseScript();
}
}
