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

#include "mongo/db/ops/parsed_delete.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_operation_source.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/parsed_writes_common.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite


namespace mongo {

// Note: The caller should hold a lock on the 'collection' so that it can stay alive until the end
// of the ParsedDelete's lifetime.
ParsedDelete::ParsedDelete(OperationContext* opCtx,
                           const DeleteRequest* request,
                           const CollectionPtr& collection,
                           bool isTimeseriesDelete)
    : _opCtx(opCtx),
      _request(request),
      _collection(collection),
      _timeseriesDeleteQueryExprs(isTimeseriesDelete
                                      ? createTimeseriesWritesQueryExprsIfNecessary(
                                            feature_flags::gTimeseriesDeletesSupport.isEnabled(
                                                serverGlobalParams.featureCompatibility),
                                            collection)
                                      : nullptr) {}

Status ParsedDelete::parseRequest() {
    dassert(!_canonicalQuery.get());
    // It is invalid to request that the DeleteStage return the deleted document during a
    // multi-remove.
    invariant(!(_request->getReturnDeleted() && _request->getMulti()));

    // It is invalid to request that a ProjectionStage be applied to the DeleteStage if the
    // DeleteStage would not return the deleted document.
    invariant(_request->getProj().isEmpty() || _request->getReturnDeleted());

    auto [collatorToUse, collationMatchesDefault] =
        resolveCollator(_opCtx, _request->getCollation(), _collection);
    _expCtx = make_intrusive<ExpressionContext>(_opCtx,
                                                std::move(collatorToUse),
                                                _request->getNsString(),
                                                _request->getLegacyRuntimeConstants(),
                                                _request->getLet());
    _expCtx->collationMatchesDefault = collationMatchesDefault;

    // The '_id' field of a time-series collection needs to be handled as other fields.
    if (CanonicalQuery::isSimpleIdQuery(_request->getQuery()) && !_timeseriesDeleteQueryExprs) {
        return Status::OK();
    }

    _expCtx->startExpressionCounters();
    return parseQueryToCQ();
}

Status ParsedDelete::parseQueryToCQ() {
    dassert(!_canonicalQuery.get());

    // The projection needs to be applied after the delete operation, so we do not specify a
    // projection during canonicalization.
    auto findCommand = std::make_unique<FindCommandRequest>(_request->getNsString());
    if (auto&& queryExprs = _timeseriesDeleteQueryExprs) {
        // If we're deleting documents from a time-series collection, splits the match expression
        // into a bucket-level match expression and a residual expression so that we can push down
        // the bucket-level match expression to the system bucket collection SCAN or FETCH/IXSCAN.
        *_timeseriesDeleteQueryExprs = timeseries::getMatchExprsForWrites(
            _expCtx, *_collection->getTimeseriesOptions(), _request->getQuery());

        // At this point, we parsed user-provided match expression. After this point, the new
        // canonical query is internal to the bucket SCAN or FETCH/IXSCAN and will have additional
        // internal match expression. We do not need to track the internal match expression counters
        // and so we stop the counters.
        _expCtx->stopExpressionCounters();

        // At least, the bucket-level filter must contain the closed bucket filter.
        tassert(7542400, "Bucket-level filter must not be null", queryExprs->_bucketExpr);
        findCommand->setFilter(queryExprs->_bucketExpr->serialize());
    } else {
        findCommand->setFilter(_request->getQuery().getOwned());
    }
    findCommand->setSort(_request->getSort().getOwned());
    findCommand->setCollation(_request->getCollation().getOwned());
    findCommand->setHint(_request->getHint());

    // Limit should only used for the findAndModify command when a sort is specified. If a sort
    // is requested, we want to use a top-k sort for efficiency reasons, so should pass the
    // limit through. Generally, a delete stage expects to be able to skip documents that were
    // deleted out from under it, but a limit could inhibit that and give an EOF when the delete
    // has not actually deleted a document. This behavior is fine for findAndModify, but should
    // not apply to deletes in general.
    if (!_request->getMulti() && !_request->getSort().isEmpty()) {
        // TODO: Due to the complexity which is related to the efficient sort support, we don't
        // support yet findAndModify with a query and sort but it should not be impossible.
        // This code assumes that in findAndModify code path, the parsed delete constructor should
        // be called with source == kTimeseriesDelete for a time-series collection.
        uassert(ErrorCodes::InvalidOptions,
                "Cannot perform a findAndModify with a query and sort on a time-series collection.",
                !_timeseriesDeleteQueryExprs);
        findCommand->setLimit(1);
    }

    // If the delete request has runtime constants or let parameters attached to it, pass them to
    // the FindCommandRequest.
    if (auto& runtimeConstants = _request->getLegacyRuntimeConstants())
        findCommand->setLegacyRuntimeConstants(*runtimeConstants);
    if (auto& letParams = _request->getLet())
        findCommand->setLet(*letParams);

    auto statusWithCQ =
        CanonicalQuery::canonicalize(_opCtx,
                                     std::move(findCommand),
                                     _request->getIsExplain(),
                                     _expCtx,
                                     ExtensionsCallbackReal(_opCtx, &_request->getNsString()),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);

    if (statusWithCQ.isOK()) {
        _canonicalQuery = std::move(statusWithCQ.getValue());
    }

    return statusWithCQ.getStatus();
}

const DeleteRequest* ParsedDelete::getRequest() const {
    return _request;
}

PlanYieldPolicy::YieldPolicy ParsedDelete::yieldPolicy() const {
    return _request->getGod() ? PlanYieldPolicy::YieldPolicy::NO_YIELD : _request->getYieldPolicy();
}

bool ParsedDelete::hasParsedQuery() const {
    return _canonicalQuery.get() != nullptr;
}

std::unique_ptr<CanonicalQuery> ParsedDelete::releaseParsedQuery() {
    invariant(_canonicalQuery.get() != nullptr);
    return std::move(_canonicalQuery);
}

bool ParsedDelete::isEligibleForArbitraryTimeseriesDelete() const {
    return _timeseriesDeleteQueryExprs && (getResidualExpr() || !_request->getMulti());
}

}  // namespace mongo
