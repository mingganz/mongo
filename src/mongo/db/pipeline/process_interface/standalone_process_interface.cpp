/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/process_interface/standalone_process_interface.h"

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/list_indexes.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builds_coordinator.h"

namespace mongo {

Status StandaloneProcessInterface::insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          const NamespaceString& ns,
                                          std::vector<BSONObj>&& objs,
                                          const WriteConcernOptions& wc,
                                          boost::optional<OID> targetEpoch) {
    auto writeResults = performInserts(
        expCtx->opCtx, buildInsertOp(ns, std::move(objs), expCtx->bypassDocumentValidation));

    // Need to check each result in the batch since the writes are unordered.
    for (const auto& result : writeResults.results) {
        if (result.getStatus() != Status::OK()) {
            return result.getStatus();
        }
    }
    return Status::OK();
}

StatusWith<MongoProcessInterface::UpdateResult> StandaloneProcessInterface::update(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& ns,
    BatchedObjects&& batch,
    const WriteConcernOptions& wc,
    UpsertType upsert,
    bool multi,
    boost::optional<OID> targetEpoch) {
    auto writeResults =
        performUpdates(expCtx->opCtx, buildUpdateOp(expCtx, ns, std::move(batch), upsert, multi));

    // Need to check each result in the batch since the writes are unordered.
    UpdateResult updateResult;
    for (const auto& result : writeResults.results) {
        if (result.getStatus() != Status::OK()) {
            return result.getStatus();
        }

        updateResult.nMatched += result.getValue().getN();
        updateResult.nModified += result.getValue().getNModified();
    }
    return updateResult;
}

std::list<BSONObj> StandaloneProcessInterface::getIndexSpecs(OperationContext* opCtx,
                                                             const NamespaceString& ns,
                                                             bool includeBuildUUIDs) {
    return listIndexesEmptyListIfMissing(opCtx, ns, includeBuildUUIDs);
}

void StandaloneProcessInterface::createIndexesOnEmptyCollection(
    OperationContext* opCtx, const NamespaceString& ns, const std::vector<BSONObj>& indexSpecs) {
    AutoGetCollection autoColl(opCtx, ns, MODE_X);
    writeConflictRetry(
        opCtx, "CommonMongodProcessInterface::createIndexesOnEmptyCollection", ns.ns(), [&] {
            auto collection = autoColl.getCollection();
            invariant(collection,
                      str::stream() << "Failed to create indexes for aggregation because "
                                       "collection does not exist: "
                                    << ns << ": " << BSON("indexes" << indexSpecs));

            invariant(0U == collection->numRecords(opCtx),
                      str::stream() << "Expected empty collection for index creation: " << ns
                                    << ": numRecords: " << collection->numRecords(opCtx) << ": "
                                    << BSON("indexes" << indexSpecs));

            // Secondary index builds do not filter existing indexes so we have to do this on the
            // primary.
            auto removeIndexBuildsToo = false;
            auto filteredIndexes = collection->getIndexCatalog()->removeExistingIndexes(
                opCtx, indexSpecs, removeIndexBuildsToo);
            if (filteredIndexes.empty()) {
                return;
            }

            WriteUnitOfWork wuow(opCtx);
            IndexBuildsCoordinator::get(opCtx)->createIndexesOnEmptyCollection(
                opCtx, collection->uuid(), filteredIndexes, false  // fromMigrate
            );
            wuow.commit();
        });
}
void StandaloneProcessInterface::renameIfOptionsAndIndexesHaveNotChanged(
    OperationContext* opCtx,
    const BSONObj& renameCommandObj,
    const NamespaceString& targetNs,
    const BSONObj& originalCollectionOptions,
    const std::list<BSONObj>& originalIndexes) {
    NamespaceString sourceNs = NamespaceString(renameCommandObj["renameCollection"].String());
    doLocalRenameIfOptionsAndIndexesHaveNotChanged(opCtx,
                                                   sourceNs,
                                                   targetNs,
                                                   renameCommandObj["dropTarget"].trueValue(),
                                                   renameCommandObj["stayTemp"].trueValue(),
                                                   originalIndexes,
                                                   originalCollectionOptions);
}

void StandaloneProcessInterface::createCollection(OperationContext* opCtx,
                                                  const std::string& dbName,
                                                  const BSONObj& cmdObj) {
    uassertStatusOK(mongo::createCollection(opCtx, dbName, cmdObj));
}

void StandaloneProcessInterface::dropCollection(OperationContext* opCtx,
                                                const NamespaceString& ns) {
    BSONObjBuilder result;
    uassertStatusOK(mongo::dropCollection(
        opCtx, ns, result, {}, DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));
}

std::unique_ptr<Pipeline, PipelineDeleter> StandaloneProcessInterface::attachCursorSourceToPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Pipeline* ownedPipeline,
    bool allowTargetingShards) {
    return attachCursorSourceToPipelineForLocalRead(expCtx, ownedPipeline);
}

}  // namespace mongo
