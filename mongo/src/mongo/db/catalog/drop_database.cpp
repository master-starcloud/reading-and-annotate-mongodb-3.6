/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/drop_database.h"

#include <algorithm>

#include "mongo/db/background.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {

// This is used to wait for the collection drops to replicate to a majority of the replica set.
// Note: Even though we're setting UNSET here, kMajority implies JOURNAL if journaling is supported
// by mongod and writeConcernMajorityJournalDefault is set to true in the ReplSetConfig.
const WriteConcernOptions kDropDatabaseWriteConcern(WriteConcernOptions::kMajority,
                                                    WriteConcernOptions::SyncMode::UNSET,
                                                    Minutes(10));

/**
 * Removes database from catalog and writes dropDatabase entry to oplog.
 */
//dropDatabase调用
Status _finishDropDatabase(OperationContext* opCtx, const std::string& dbName, Database* db) {
    // If Database::dropDatabase() fails, we should reset the drop-pending state on Database.
    auto dropPendingGuard = MakeGuard([db, opCtx] { db->setDropPending(opCtx, false); });

	//DatabaseImpl::dropDatabase
    Database::dropDatabase(opCtx, db);
    dropPendingGuard.Dismiss();

    log() << "dropDatabase " << dbName << " - finished";

    WriteUnitOfWork wunit(opCtx);
	//OpObserverImpl::onDropDatabase
    getGlobalServiceContext()->getOpObserver()->onDropDatabase(opCtx, dbName);
    wunit.commit();

    return Status::OK();
}

}  // namespace


//drop_database.cpp中的dropDatabase和DatabaseImpl::dropDatabase  dropDatabaseImpl什么区别？需要进一步分析
//区别如下：drop_database.cpp中的dropDatabase会通过_finishDropDatabase调用DatabaseImpl::dropDatabase
//drop_database.cpp中的dropDatabase删库及其库下面的表，DatabaseImpl::dropDatabase只删库

//CmdDropDatabase::run 调用
Status dropDatabase(OperationContext* opCtx, const std::string& dbName) {
    uassert(ErrorCodes::IllegalOperation,
            "Cannot drop a database in read-only mode",
            !storageGlobalParams.readOnly);
    // TODO (Kal): OldClientContext legacy, needs to be removed
    {
        CurOp::get(opCtx)->ensureStarted();
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setNS_inlock(dbName);
    }

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    std::size_t numCollectionsToDrop = 0;

    // We have to wait for the last drop-pending collection to be removed if there are no
    // collections to drop.
    repl::OpTime latestDropPendingOpTime;

    using Result = boost::optional<Status>;
    // Get an optional result--if it's there, early return; otherwise, wait for collections to drop.
    //先删表
    auto result = writeConflictRetry(opCtx, "dropDatabase_collection", dbName, [&] {
        Lock::GlobalWrite lk(opCtx);
        AutoGetDb autoDB(opCtx, dbName, MODE_X);
        Database* const db = autoDB.getDb();
        if (!db) {
            return Result(Status(ErrorCodes::NamespaceNotFound,
                                 str::stream() << "Could not drop database " << dbName
                                               << " because it does not exist"));
        }

        bool userInitiatedWritesAndNotPrimary =
            opCtx->writesAreReplicated() && !replCoord->canAcceptWritesForDatabase(opCtx, dbName);

        if (userInitiatedWritesAndNotPrimary) {
            return Result(
                Status(ErrorCodes::NotMaster,
                       str::stream() << "Not primary while dropping database " << dbName));
        }

        log() << "dropDatabase " << dbName << " - starting";
        db->setDropPending(opCtx, true);

        // If Database::dropCollectionEventIfSystem() fails, we should reset the drop-pending state
        // on Database.
        auto dropPendingGuard = MakeGuard([&db, opCtx] { db->setDropPending(opCtx, false); });

		//删除DB下面所有的表
        for (auto collection : *db) {
            const auto& nss = collection->ns();
            if (nss.isDropPendingNamespace() && replCoord->isReplEnabled() &&
                opCtx->writesAreReplicated()) {
                log() << "dropDatabase " << dbName << " - found drop-pending collection: " << nss;
                latestDropPendingOpTime = std::max(
                    latestDropPendingOpTime, uassertStatusOK(nss.getDropPendingNamespaceOpTime()));
                continue;
            }
            if (replCoord->isOplogDisabledFor(opCtx, nss) || nss.isSystemDotIndexes()) {
                continue;
            }
            log() << "dropDatabase " << dbName << " - dropping collection: " << nss;
            WriteUnitOfWork wunit(opCtx);
			//DatabaseImpl::dropCollectionEvenIfSystem
            fassertStatusOK(40476, db->dropCollectionEvenIfSystem(opCtx, nss));
            wunit.commit();
            numCollectionsToDrop++;
        }
        dropPendingGuard.Dismiss();

        // If there are no collection drops to wait for, we complete the drop database operation.
        //如果库下面没有表信息，则直接删库
        if (numCollectionsToDrop == 0U && latestDropPendingOpTime.isNull()) {
            return Result(_finishDropDatabase(opCtx, dbName, db));
        }

        return Result(boost::none);
    });

	//获取删表结果
    if (result) {
        return *result;
    }

    // If waitForWriteConcern() returns an error or throws an exception, we should reset the
    // drop-pending state on Database.
    auto dropPendingGuardWhileAwaitingReplication = MakeGuard([dbName, opCtx] {
        Lock::GlobalWrite lk(opCtx);
        AutoGetDb autoDB(opCtx, dbName, MODE_X);
        if (auto db = autoDB.getDb()) {
            db->setDropPending(opCtx, false);
        }
    });

    {
        // Holding of any locks is disallowed while awaiting replication because this can
        // potentially block for long time while doing network activity.
        //
        // Even though dropDatabase() does not explicitly acquire any locks before awaiting
        // replication, it is possible that the caller of this function may already have acquired
        // a lock. The applyOps command is an example of a dropDatabase() caller that does this.
        // Therefore, we have to release any locks using a TempRelease RAII object.
        //
        // TODO: Remove the use of this TempRelease object when SERVER-29802 is completed.
        // The work in SERVER-29802 will adjust the locking rules around applyOps operations and
        // dropDatabase is expected to be one of the operations where we expect to no longer acquire
        // the global lock.
        Lock::TempRelease release(opCtx->lockState());

        if (numCollectionsToDrop > 0U) {
			//等等副本集从节点的表也要删除
            auto status =
                replCoord->awaitReplicationOfLastOpForClient(opCtx, kDropDatabaseWriteConcern)
                    .status;
            if (!status.isOK()) {
                return Status(status.code(),
                              str::stream() << "dropDatabase " << dbName << " failed waiting for "
                                            << numCollectionsToDrop
                                            << " collection drops to replicate: "
                                            << status.reason());
            }

            log() << "dropDatabase " << dbName << " - successfully dropped " << numCollectionsToDrop
                  << " collections. dropping database";
        } else {
            invariant(!latestDropPendingOpTime.isNull());
            auto status =
                replCoord
                    ->awaitReplication(opCtx, latestDropPendingOpTime, kDropDatabaseWriteConcern)
                    .status;
            if (!status.isOK()) {
                return Status(
                    status.code(),
                    str::stream()
                        << "dropDatabase "
                        << dbName
                        << " failed waiting for pending collection drops (most recent drop optime: "
                        << latestDropPendingOpTime.toString()
                        << ") to replicate: "
                        << status.reason());
            }

            log() << "dropDatabase " << dbName
                  << " - pending collection drops completed. dropping database";
        }
    }

    dropPendingGuardWhileAwaitingReplication.Dismiss();

	//前面把库下面的所有表删除干净后，可以开始删库了
    return writeConflictRetry(opCtx, "dropDatabase_database", dbName, [&] {
        Lock::GlobalWrite lk(opCtx);

        bool userInitiatedWritesAndNotPrimary =
            opCtx->writesAreReplicated() && !replCoord->canAcceptWritesForDatabase(opCtx, dbName);

        if (userInitiatedWritesAndNotPrimary) {
            return Status(ErrorCodes::PrimarySteppedDown,
                          str::stream() << "Could not drop database " << dbName
                                        << " because we transitioned from PRIMARY to "
                                        << replCoord->getMemberState().toString()
                                        << " while waiting for "
                                        << numCollectionsToDrop
                                        << " pending collection drop(s).");
        }

        AutoGetDb autoDB(opCtx, dbName, MODE_X);
        if (auto db = autoDB.getDb()) {
            return _finishDropDatabase(opCtx, dbName, db);
        }

        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Could not drop database " << dbName
                                    << " because it does not exist after dropping "
                                    << numCollectionsToDrop
                                    << " collection(s).");
    });
}

}  // namespace mongo
