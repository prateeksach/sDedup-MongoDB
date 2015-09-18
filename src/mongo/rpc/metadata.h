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

#pragma once

#include <tuple>

#include "mongo/base/status_with.h"

namespace mongo {
    class BSONObj;

/**
 * Utilities for converting metadata between the legacy OP_QUERY format and the new
 * OP_COMMAND format.
 *
 * Metadata consists of information independent of any particular command such as:
 *
 * Request/Reply/Both | (legacy) OP_QUERY format         | OP_COMMAND format
 *__________________________________________________________________________________________________
 * Request            | the slaveOk bit                  | $secondaryOk on metadata obj
 * Request            | $readPreference field of command | $readPreference on metadata obj
 * Request            | $impersonatedUsers on command obj| $impersonatedUsers on metadata obj
 * Request            | $impersonatedRoles on command obj| $impersonatedRoles on metadata obj
 * Request            | maxTimeMS on command obj         | $maxTimeMS on metadata obj
 * Reply              | $gleStats field on command reply | $gleStats on metadata obj
 *
 * TODO: currently only $secondaryOk (request only) is handled. SERVER-18236 will cover the rest.
 */
namespace rpc {
namespace metadata {

    /**
     * Returns an empty metadata object.
     */
    BSONObj empty();

    /**
     * The field name for the secondaryOk metadata field.
     */
    extern const char kSecondaryOk[];

    /**
     * A command object and a corresponding metadata object.
     */
    using CommandAndMetadata = std::tuple<BSONObj, BSONObj>;

    /**
     * A legacy command object and a corresponding query flags bitfield. The legacy command object
     * may contain metadata fields, so it cannot safely be passed to a command's run method.
     */
    using LegacyCommandAndFlags = std::tuple<BSONObj, int>;

    /**
     * Given a legacy command object and a query flags bitfield, attempts to parse and remove
     * the metadata from the command object and construct a corresponding metadata object.
     */
    StatusWith<CommandAndMetadata> upconvertRequest(BSONObj legacyCmdObj, int queryFlags);

    /**
     * Given a command object and a metadata object, attempts to construct a legacy command
     * object and query flags bitfield augmented with the given metadata.
     */
    StatusWith<LegacyCommandAndFlags> downconvertRequest(BSONObj cmdObj, BSONObj metadata);

}  // namespace metadata
}  // namespace rpc
}  // namespace mongo
