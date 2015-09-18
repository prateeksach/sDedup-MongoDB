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

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/dist_lock_catalog_impl.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/find_and_modify_request.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_runner.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/lasterror.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/type_lockpings.h"
#include "mongo/s/type_locks.h"
#include "mongo/s/write_ops/wc_error_detail.h"
#include "mongo/util/time_support.h"

namespace mongo {

    using std::string;

namespace {

    const char kCmdResponseWriteConcernField[] = "writeConcernError";
    const char kFindAndModifyResponseResultDocField[] = "value";
    const char kLocalTimeField[] = "localTime";
    const ReadPreferenceSetting kReadPref(ReadPreference::PrimaryOnly, TagSet());

    /**
     * Returns the resulting new object from the findAndModify response object.
     * This also checks for errors in the response object.
     */
    StatusWith<BSONObj> extractFindAndModifyNewObj(const BSONObj& responseObj) {
        auto cmdStatus = getStatusFromCommandResult(responseObj);

        if (!cmdStatus.isOK()) {
            return cmdStatus;
        }

        BSONElement wcErrorElem;
        auto wcErrStatus = bsonExtractTypedField(responseObj,
                                                 kCmdResponseWriteConcernField,
                                                 Object,
                                                 &wcErrorElem);

        if (wcErrStatus.isOK()) {
            BSONObj wcErrObj(wcErrorElem.Obj());
            WCErrorDetail wcError;

            string wcErrorParseMsg;
            if (!wcError.parseBSON(wcErrObj, &wcErrorParseMsg)) {
                return Status(ErrorCodes::UnsupportedFormat, wcErrorParseMsg);
            }

            return {ErrorCodes::WriteConcernFailed, wcError.getErrMessage()};
        }

        if (wcErrStatus.code() != ErrorCodes::NoSuchKey) {
            return wcErrStatus;
        }

        if (const auto& newDocElem = responseObj[kFindAndModifyResponseResultDocField]) {
            if (newDocElem.isNull()) {
                // For cases when nMatched == 0.
                return BSONObj();
            }

            if (!newDocElem.isABSONObj()) {
                return {ErrorCodes::UnsupportedFormat,
                        "expected an object from the findAndModify response 'value' field"};
            }

            return newDocElem.Obj();
        }

        return BSONObj();
    }

    /**
     * Extract the electionId from a command response.
     */
    StatusWith<OID> extractElectionId(const BSONObj& responseObj) {
        BSONElement gleStatsElem;
        auto gleStatus = bsonExtractTypedField(responseObj,
                                               kGLEStatsFieldName,
                                               Object,
                                               &gleStatsElem);

        if (!gleStatus.isOK()) {
            return {ErrorCodes::UnsupportedFormat, gleStatus.reason()};
        }

        OID electionId;

        auto electionIdStatus = bsonExtractOIDField(gleStatsElem.Obj(),
                                                    kGLEStatsElectionIdFieldName,
                                                    &electionId);

        if (!electionIdStatus.isOK()) {
            return {ErrorCodes::UnsupportedFormat, electionIdStatus.reason()};
        }

        return electionId;
    }

} // unnamed namespace

    DistLockCatalogImpl::DistLockCatalogImpl(RemoteCommandTargeter* targeter,
                                             RemoteCommandRunner* executor,
                                             Milliseconds writeConcernTimeout):
        _cmdRunner(executor),
        _targeter(targeter),
        _writeConcern(WriteConcernOptions(WriteConcernOptions::kMajority,
                                          WriteConcernOptions::JOURNAL,
                                          writeConcernTimeout.count())),
        _lockPingNS(LockpingsType::ConfigNS),
        _locksNS(LocksType::ConfigNS) {
    }

    DistLockCatalogImpl::~DistLockCatalogImpl() = default;

    StatusWith<LockpingsType> DistLockCatalogImpl::getPing(StringData processID) {
        invariant(false);
        return {ErrorCodes::InternalError, "not yet implemented"};
    }

    Status DistLockCatalogImpl::ping(StringData processID, Date_t ping) {
        auto targetStatus = _targeter->findHost(kReadPref);

        if (!targetStatus.isOK()) {
            return targetStatus.getStatus();
        }

        auto request = FindAndModifyRequest::makeUpdate(_lockPingNS,
                BSON(LockpingsType::process(processID.toString())),
                BSON("$set" << BSON(LockpingsType::ping(ping))));
        request.setUpsert(true);
        request.setWriteConcern(_writeConcern);

        auto resultStatus = _cmdRunner->runCommand(
                RemoteCommandRequest(targetStatus.getValue(),
                                     _locksNS.db().toString(),
                                     request.toBSON()));

        if (!resultStatus.isOK()) {
            return resultStatus.getStatus();
        }

        const RemoteCommandResponse& response = resultStatus.getValue();
        BSONObj responseObj(response.data);

        auto cmdStatus = getStatusFromCommandResult(responseObj);

        if (!cmdStatus.isOK()) {
            return cmdStatus;
        }

        auto findAndModifyStatus = extractFindAndModifyNewObj(responseObj);
        return findAndModifyStatus.getStatus();
    }

    StatusWith<LocksType> DistLockCatalogImpl::grabLock(StringData lockID,
                                                        const OID& lockSessionID,
                                                        StringData who,
                                                        StringData processId,
                                                        Date_t time,
                                                        StringData why) {
        auto targetStatus = _targeter->findHost(kReadPref);

        if (!targetStatus.isOK()) {
            return targetStatus.getStatus();
        }

        BSONObj newLockDetails(BSON(LocksType::lockID(lockSessionID)
                                    << LocksType::state(LocksType::LOCKED)
                                    << LocksType::who() << who
                                    << LocksType::process() << processId
                                    << LocksType::when(time)
                                    << LocksType::why() << why));

        auto request = FindAndModifyRequest::makeUpdate(_locksNS,
                BSON(LocksType::name() << lockID << LocksType::state(LocksType::UNLOCKED)),
                BSON("$set" << newLockDetails));
        request.setUpsert(true);
        request.setShouldReturnNew(true);
        request.setWriteConcern(_writeConcern);

        auto resultStatus = _cmdRunner->runCommand(
                RemoteCommandRequest(targetStatus.getValue(),
                                     _locksNS.db().toString(),
                                     request.toBSON()));

        if (!resultStatus.isOK()) {
            return resultStatus.getStatus();
        }

        const RemoteCommandResponse& response = resultStatus.getValue();
        BSONObj responseObj(response.data);

        auto cmdStatus = getStatusFromCommandResult(responseObj);

        if (!cmdStatus.isOK()) {
            return cmdStatus;
        }

        auto findAndModifyStatus = extractFindAndModifyNewObj(responseObj);
        if (!findAndModifyStatus.isOK()) {
            return findAndModifyStatus.getStatus();
        }

        BSONObj newDoc(findAndModifyStatus.getValue());

        if (newDoc.isEmpty()) {
            return LocksType();
        }

        LocksType lockDoc;
        string errMsg;
        if (!lockDoc.parseBSON(newDoc, &errMsg)) {
            return {ErrorCodes::FailedToParse, errMsg};
        }

        return lockDoc;
    }

    StatusWith<LocksType> DistLockCatalogImpl::overtakeLock(StringData lockID,
                                                            const OID& lockSessionID,
                                                            const OID& currentHolderTS,
                                                            StringData who,
                                                            StringData processId,
                                                            Date_t time,
                                                            StringData why) {
        auto targetStatus = _targeter->findHost(kReadPref);

        if (!targetStatus.isOK()) {
            return targetStatus.getStatus();
        }

        BSONArrayBuilder orQueryBuilder;
        orQueryBuilder.append(BSON(LocksType::name() << lockID
                                    << LocksType::state(LocksType::UNLOCKED)));
        orQueryBuilder.append(BSON(LocksType::name() << lockID << LocksType::lockID(currentHolderTS)));

        BSONObj newLockDetails(BSON(LocksType::lockID(lockSessionID)
                                    << LocksType::state(LocksType::LOCKED)
                                    << LocksType::who() << who
                                    << LocksType::process() << processId
                                    << LocksType::when(time)
                                    << LocksType::why() << why));

        auto request = FindAndModifyRequest::makeUpdate(_locksNS,
                BSON("$or" << orQueryBuilder.arr()),
                BSON("$set" << newLockDetails));
        request.setShouldReturnNew(true);
        request.setWriteConcern(_writeConcern);

        auto resultStatus = _cmdRunner->runCommand(
                RemoteCommandRequest(targetStatus.getValue(),
                                     _locksNS.db().toString(),
                                     request.toBSON()));

        if (!resultStatus.isOK()) {
            return resultStatus.getStatus();
        }

        const RemoteCommandResponse& response = resultStatus.getValue();
        BSONObj responseObj(response.data);

        auto findAndModifyStatus = extractFindAndModifyNewObj(responseObj);
        if (!findAndModifyStatus.isOK()) {
            return findAndModifyStatus.getStatus();
        }

        BSONObj newDoc(findAndModifyStatus.getValue());

        if (newDoc.isEmpty()) {
            return LocksType();
        }

        LocksType lockDoc;
        string errMsg;
        if (!lockDoc.parseBSON(newDoc, &errMsg)) {
            return {ErrorCodes::FailedToParse, errMsg};
        }

        return lockDoc;
    }

    Status DistLockCatalogImpl::unlock(const OID& lockSessionID) {
        auto targetStatus = _targeter->findHost(kReadPref);

        if (!targetStatus.isOK()) {
            return targetStatus.getStatus();
        }

        auto request = FindAndModifyRequest::makeUpdate(_locksNS,
                BSON(LocksType::lockID(lockSessionID)),
                BSON("$set" << BSON(LocksType::state(LocksType::UNLOCKED))));
        request.setWriteConcern(_writeConcern);

        auto resultStatus = _cmdRunner->runCommand(
                RemoteCommandRequest(targetStatus.getValue(),
                                     _locksNS.db().toString(),
                                     request.toBSON()));

        if (!resultStatus.isOK()) {
            return resultStatus.getStatus();
        }

        const RemoteCommandResponse& response = resultStatus.getValue();
        BSONObj responseObj(response.data);

        auto findAndModifyStatus = extractFindAndModifyNewObj(responseObj);
        return findAndModifyStatus.getStatus();
    }

    StatusWith<DistLockCatalog::ServerInfo> DistLockCatalogImpl::getServerInfo() {
        auto targetStatus = _targeter->findHost(kReadPref);

        if (!targetStatus.isOK()) {
            return targetStatus.getStatus();
        }

        auto resultStatus = _cmdRunner->runCommand(RemoteCommandRequest(
                targetStatus.getValue(),
                "admin",
                BSON("serverStatus" << 1)));

        if (!resultStatus.isOK()) {
            return resultStatus.getStatus();
        }

        const RemoteCommandResponse& response = resultStatus.getValue();
        BSONObj responseObj(response.data);

        auto cmdStatus = getStatusFromCommandResult(responseObj);

        if (!cmdStatus.isOK()) {
            return cmdStatus;
        }

        BSONElement localTimeElem;
        auto localTimeStatus = bsonExtractTypedField(responseObj,
                                                     kLocalTimeField,
                                                     Date,
                                                     &localTimeElem);

        if (!localTimeStatus.isOK()) {
            return {ErrorCodes::UnsupportedFormat, localTimeStatus.reason()};
        }

        auto electionIdStatus = extractElectionId(responseObj);

        if (!electionIdStatus.isOK()) {
            return {ErrorCodes::UnsupportedFormat, electionIdStatus.getStatus().reason()};
        }

        return DistLockCatalog::ServerInfo(localTimeElem.date(), electionIdStatus.getValue());
    }

    StatusWith<LocksType> DistLockCatalogImpl::getLockByTS(const OID& ts) {
        invariant(false);
        return {ErrorCodes::InternalError, "not yet implemented"};
    }

} // namespace mongo
