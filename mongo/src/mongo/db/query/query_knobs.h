/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/platform/atomic_proxy.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

//
// multi-plan ranking
//

// Max number of times we call work() on plans before comparing them,
// for small collections.  getTrialPeriodWorks中使用
extern AtomicInt32 internalQueryPlanEvaluationWorks;

// For large collections, the number times we work() candidate plans is
// taken as this fraction of the collection size.
extern AtomicDouble internalQueryPlanEvaluationCollFraction;

// Stop working plans once a plan returns this many results.
//默认101
extern AtomicInt32 internalQueryPlanEvaluationMaxResults;

// Do we give a big ranking bonus to intersection plans?
extern AtomicBool internalQueryForceIntersectionPlans;

// Do we have ixisect on at all?
extern AtomicBool internalQueryPlannerEnableIndexIntersection;

// Do we use hash-based intersection for rooted $and queries?
extern AtomicBool internalQueryPlannerEnableHashIntersection;

//
// plan cache
//

// How many entries in the cache?
extern AtomicInt32 internalQueryCacheSize;

// How many feedback entries do we collect before possibly evicting from the cache based on bad
// performance?
extern AtomicInt32 internalQueryCacheFeedbacksStored;

// How many times more works must we perform in order to justify plan cache eviction
// and replanning?
extern AtomicDouble internalQueryCacheEvictionRatio;

//
// Planning and enumeration.
//

// How many indexed solutions will QueryPlanner::plan output?
//MONGO_EXPORT_SERVER_PARAMETER(internalQueryPlannerMaxIndexedSolutions, int, 64);
extern AtomicInt32 internalQueryPlannerMaxIndexedSolutions;

// How many solutions will the enumerator consider at each OR?
extern AtomicInt32 internalQueryEnumerationMaxOrSolutions;

// How many intersections will the enumerator consider at each AND?
extern AtomicInt32 internalQueryEnumerationMaxIntersectPerAnd;

// Do we want to plan each child of the OR independently?
extern AtomicBool internalQueryPlanOrChildrenIndependently;

// How many index scans are we willing to produce in order to obtain a sort order
// during explodeForSort?
extern AtomicInt32 internalQueryMaxScansToExplode;

// Allow the planner to generate covered whole index scans, rather than falling back to a COLLSCAN.
extern AtomicBool internalQueryPlannerGenerateCoveredWholeIndexScans;

// Ignore unknown JSON Schema keywords.
extern AtomicBool internalQueryIgnoreUnknownJSONSchemaKeywords;

//
// Query execution.
//

extern AtomicInt32 internalQueryExecMaxBlockingSortBytes;

// Yield after this many "should yield?" checks.
//调用累积次数超过该配置值会主动 yield，默认为 128，本质上反映的是从索引或者表上获取
//了多少条数据后主动 yield。yield 之后该累积次数清零。
extern AtomicInt32 internalQueryExecYieldIterations;

//internalQueryExecYieldPeriodMS 可以通过setParameter 命令配置，控制查询命令的AutoYield的间隔，默认10毫秒。
// Yield if it's been at least this many milliseconds since we last yielded.
//从上次 yield 到现在的时间间隔超过该配置值，主动 yield，默认为 10ms，
//本质上反映的是当前线程获取数据的行为持续了多久需要 yield。
extern AtomicInt32 internalQueryExecYieldPeriodMS;

// Limit the size that we write without yielding to 16MB / 64 (max expected number of indexes)
const int64_t insertVectorMaxBytes = 256 * 1024;

// The number of bytes to buffer at once during a $facet stage.
extern AtomicInt32 internalQueryFacetBufferSizeBytes;
//AtomicInt32类型变量通过internalInsertMaxBatchSize.load()访问
extern AtomicInt32 internalInsertMaxBatchSize;

extern AtomicInt32 internalDocumentSourceCursorBatchSizeBytes;

extern AtomicInt32 internalDocumentSourceLookupCacheSizeBytes;

extern AtomicBool internalQueryProhibitBlockingMergeOnMongoS;
}  // namespace mongo
