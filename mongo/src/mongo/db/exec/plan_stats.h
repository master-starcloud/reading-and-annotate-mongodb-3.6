/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
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

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * The interface all specific-to-stage stats provide.
 */ 

/*
Stats:
{ "stage" : "LIMIT",
  "nReturned" : 3,
  "executionTimeMillisEstimate" : 0,
  "works" : 3,
  "advanced" : 3,
  "needTime" : 0,
  "needYield" : 0,
  "saveState" : 0,
  "restoreState" : 0,
  "isEOF" : 1,
  "invalidates" : 0,
  "limitAmount" : 3,
  "inputStage" : { "stage" : "PROJECTION",
    "nReturned" : 3,
    "executionTimeMillisEstimate" : 0,
    "works" : 3,
    "advanced" : 3,
    "needTime" : 0,
    "needYield" : 0,
    "saveState" : 0,
    "restoreState" : 0,
    "isEOF" : 0,
    "invalidates" : 0,
    "transformBy" : { "age" : 1 },
    "inputStage" : { "stage" : "FETCH",
      "nReturned" : 3,
      "executionTimeMillisEstimate" : 0,
      "works" : 3,
      "advanced" : 3,
      "needTime" : 0,
      "needYield" : 0,
      "saveState" : 0,
      "restoreState" : 0,
      "isEOF" : 0,
      "invalidates" : 0,
      "docsExamined" : 3,
      "alreadyHasObj" : 0,
      "inputStage" : { "stage" : "IXSCAN",
        "nReturned" : 3,
        "executionTimeMillisEstimate" : 0,
        "works" : 3,
        "advanced" : 3,
        "needTime" : 0,
        "needYield" : 0,
        "saveState" : 0,
        "restoreState" : 0,
        "isEOF" : 0,
        "invalidates" : 0,
        "keyPattern" : { "name" : 1,
          "age" : 1 },
        "indexName" : "name_1_age_1",
        "isMultiKey" : false,
        "multiKeyPaths" : { "name" : [],
          "age" : [] },
        "isUnique" : false,
        "isSparse" : false,
        "isPartial" : false,
        "indexVersion" : 2,
        "direction" : "forward",
        "indexBounds" : { "name" : [ 
            "[\"yangyazhou2\", \"yangyazhou2\"]" ],
          "age" : [ 
            "[MinKey, MaxKey]" ] },
        "keysExamined" : 3,
        "seeks" : 1,
        "dupsTested" : 0,
        "dupsDropped" : 0,
        "seenInvalidated" : 0 } } } }
*/
//下面的AndHashStats AndSortedStats等继承该类    PlanStageStats.specific为该类型
//每个stage都有对应的统计，例如CollectionScanStats为CollectionScan stage对应统计信息，
//MultiPlanStats为MultiPlanStage stage对应统计信息
struct SpecificStats {
    virtual ~SpecificStats() {}

    /**
     * Make a deep copy.
     */
    virtual SpecificStats* clone() const = 0;
};

// Every stage has CommonStats. 
//PlanStageStats输出打印可以参考PlanCacheListPlans::list  PlanRanker::pickBestPlan

//PlanStageStats.common为该类型
struct CommonStats { //算分用的该类，参考PlanStage::work    explain("allPlansExecution")展示的内容来源就在该结构
    CommonStats(const char* type)
        : stageTypeStr(type),
          works(0),
          yields(0),
          unyields(0),
          invalidates(0),
          advanced(0),
          needTime(0),
          needYield(0),
          executionTimeMillis(0),
          isEOF(false) {}
    // String giving the type of the stage. Not owned.
    const char* stageTypeStr;

    // Count calls into the stage.
    //works则是总共扫描的次数
    size_t works; //PlanStage::work中赋值，总次数 下面的advanced  needTime  needYield总和
    size_t yields;
    size_t unyields;
    size_t invalidates;

    // How many times was this state the return value of work(...)?
    //common.advanced是每个索引扫描的时候是否能在collection拿到符合条件的记录，
    //如果能拿到记录那么common.advanced就加1，
    size_t advanced;//赋值见PlanStage::work PlanRanker::scoreTree算分的时候，会用到
    size_t needTime;//赋值见PlanStage::work
    size_t needYield; //赋值见PlanStage::work

    // BSON representation of a MatchExpression affixed to this node. If there
    // is no filter affixed, then 'filter' should be an empty BSONObj.
    BSONObj filter;

    // Time elapsed while working inside this stage.
    long long executionTimeMillis;

    // TODO: have some way of tracking WSM sizes (or really any series of #s).  We can measure
    // the size of our inputs and the size of our outputs.  We can do a lot with the WS here.

    // TODO: once we've picked a plan, collect different (or additional) stats for display to
    // the user, eg. time_t totalTimeSpent;

    // TODO: keep track of the total yield time / fetch time done for a plan.
    //在MongoDB扫描A次的时候，如果某个索引的命中数量小于A次，那它必然会提前扫描完，然后标志位状态为IS_EOF
    //参考https://segmentfault.com/a/1190000015236644
    //参考PlanRanker::pickBestPlan
    //如果状态为IS_EOF则加一分，所以一般达到IS_EOF状态的索引都会被选中为最优执行计划。
    bool isEOF;

private:
    // Default constructor is illegal.
    CommonStats();
};

// The universal container for a stage's stats.
//PlanStageStats输出打印可以参考PlanCacheListPlans::list  PlanRanker::pickBestPlan

//MultiPlanStage::getStats()中构造使用，使用方法可以参考MultiPlanStage::getStats()
//以MultiPlanStage为例，使用参考MultiPlanStage::getStats()
//每个stage有个对应的PlanStageStats统计，这些统计真正在PlanRanker::pickBestPlan使用，目的是选择最优索引

//PlanRankingDecision.stats成员为该类型
struct PlanStageStats {
    PlanStageStats(const CommonStats& c, StageType t) : stageType(t), common(c) {}

    /**
     * Make a deep copy.
     */
    PlanStageStats* clone() const {
        PlanStageStats* stats = new PlanStageStats(common, stageType);
        if (specific.get()) {
            stats->specific.reset(specific->clone());
        }
        for (size_t i = 0; i < children.size(); ++i) {
            invariant(children[i]);
            stats->children.emplace_back(children[i]->clone());
        }
        return stats;
    }

    // See query/stage_type.h
    StageType stageType;

    // Stats exported by implementing the PlanStage interface.
    //算分用的该类，参考PlanStage::work 
    CommonStats common;  

    // Per-stage place to stash additional information
    //以MultiPlanStage为例，使用参考MultiPlanStage::getStats()
    std::unique_ptr<SpecificStats> specific;

    // The stats of the node's children.
    std::vector<std::unique_ptr<PlanStageStats>> children;

private:
    MONGO_DISALLOW_COPYING(PlanStageStats);
};

struct AndHashStats : public SpecificStats {
    AndHashStats() : flaggedButPassed(0), flaggedInProgress(0), memUsage(0), memLimit(0) {}

    SpecificStats* clone() const final {
        AndHashStats* specific = new AndHashStats(*this);
        return specific;
    }

    // Invalidation counters.
    // How many results had the AND fully evaluated but were invalidated?
    size_t flaggedButPassed;

    // How many results were mid-AND but got flagged?
    size_t flaggedInProgress;

    // How many entries are in the map after each child?
    // child 'i' produced children[i].common.advanced RecordIds, of which mapAfterChild[i] were
    // intersections.
    std::vector<size_t> mapAfterChild;

    // mapAfterChild[mapAfterChild.size() - 1] WSMswere match tested.
    // commonstats.advanced is how many passed.

    // What's our current memory usage?
    size_t memUsage;

    // What's our memory limit?
    size_t memLimit;
};


struct AndSortedStats : public SpecificStats {
    AndSortedStats() : flagged(0) {}

    SpecificStats* clone() const final {
        AndSortedStats* specific = new AndSortedStats(*this);
        return specific;
    }

    // How many results from each child did not pass the AND?
    std::vector<size_t> failedAnd;

    // How many results were flagged via invalidation?
    size_t flagged;
};

struct CachedPlanStats : public SpecificStats {
    CachedPlanStats() : replanned(false) {}

    SpecificStats* clone() const final {
        return new CachedPlanStats(*this);
    }

    bool replanned;
};

//CollectionScan对应stage的统计
struct CollectionScanStats : public SpecificStats {
    CollectionScanStats() : docsTested(0), direction(1) {}

    SpecificStats* clone() const final {
        CollectionScanStats* specific = new CollectionScanStats(*this);
        return specific;
    }

    // How many documents did we check against our filter?
    size_t docsTested;

    // >0 if we're traversing the collection forwards. <0 if we're traversing it
    // backwards.
    //query的查询顺序，此处是forward，如果用了.sort({w:-1})将显示backward。
    int direction;

    // If present, indicates that the collection scan will stop and return EOF the first time it
    // sees a document that does not pass the filter and has a "ts" Timestamp field greater than
    // 'maxTs'.
    boost::optional<Timestamp> maxTs;
};

struct CountStats : public SpecificStats {
    CountStats() : nCounted(0), nSkipped(0), recordStoreCount(false) {}

    SpecificStats* clone() const final {
        CountStats* specific = new CountStats(*this);
        return specific;
    }

    // The result of the count.
    long long nCounted;

    // The number of results we skipped over.
    long long nSkipped;

    // True if we computed the count via Collection::numRecords().
    bool recordStoreCount;
};

struct CountScanStats : public SpecificStats {
    CountScanStats()
        : indexVersion(0),
          isMultiKey(false),
          isPartial(false),
          isSparse(false),
          isUnique(false),
          keysExamined(0) {}

    SpecificStats* clone() const final {
        CountScanStats* specific = new CountScanStats(*this);
        // BSON objects have to be explicitly copied.
        specific->keyPattern = keyPattern.getOwned();
        specific->collation = collation.getOwned();
        specific->startKey = startKey.getOwned();
        specific->endKey = endKey.getOwned();
        return specific;
    }

    std::string indexName;

    BSONObj keyPattern;

    BSONObj collation;

    // The starting/ending key(s) of the index scan.
    // startKey and endKey contain the fields of keyPattern, with values
    // that match the corresponding index bounds.
    //例如查询条件是w:1,使用的index是w与n的联合索引，故w是[1.0,1.0]而n没有指定在查询条件中，
    //故是[MinKey,MaxKey]。     explain.queryPlanner.winningPlan.indexBounds显示
    BSONObj startKey;
    BSONObj endKey;
    // Whether or not those keys are inclusive or exclusive bounds.
    bool startKeyInclusive;
    bool endKeyInclusive;

    int indexVersion;

    // Set to true if the index used for the count scan is multikey.
    /*
    explain.queryPlanner.winningPlan.isMultiKey
    是否是Multikey，此处返回是false，如果索引建立在array上，此处将是true。
     */
    bool isMultiKey;

    // Represents which prefixes of the indexed field(s) cause the index to be multikey.
    MultikeyPaths multiKeyPaths;

    bool isPartial;
    bool isSparse;
    bool isUnique;

    //1个走索引的查询， 扫描了多少条索引， 可查看 system.profile 的 keysExamined 字段， 该值越大， CPU 开销越大
    size_t keysExamined;
};

struct DeleteStats : public SpecificStats {
    DeleteStats() : docsDeleted(0), nInvalidateSkips(0) {}

    SpecificStats* clone() const final {
        return new DeleteStats(*this);
    }

    size_t docsDeleted;

    // Invalidated documents can be force-fetched, causing the now invalid RecordId to
    // be thrown out. The delete stage skips over any results which do not have a RecordId.
    size_t nInvalidateSkips;
};

struct DistinctScanStats : public SpecificStats {
    SpecificStats* clone() const final {
        DistinctScanStats* specific = new DistinctScanStats(*this);
        // BSON objects have to be explicitly copied.
        specific->keyPattern = keyPattern.getOwned();
        specific->collation = collation.getOwned();
        specific->indexBounds = indexBounds.getOwned();
        return specific;
    }

    // How many keys did we look at while distinct-ing?
    size_t keysExamined = 0;

    BSONObj keyPattern;

    BSONObj collation;

    // Properties of the index used for the distinct scan.
    std::string indexName;
    int indexVersion = 0;

    // Set to true if the index used for the distinct scan is multikey.
    bool isMultiKey = false;

    // Represents which prefixes of the indexed field(s) cause the index to be multikey.
    MultikeyPaths multiKeyPaths;

    bool isPartial = false;
    bool isSparse = false;
    bool isUnique = false;

    // >1 if we're traversing the index forwards and <1 if we're traversing it backwards.
    int direction = 1;

    // A BSON representation of the distinct scan's index bounds.
    BSONObj indexBounds;
};

struct EnsureSortedStats : public SpecificStats {
    EnsureSortedStats() : nDropped(0) {}

    SpecificStats* clone() const final {
        EnsureSortedStats* specific = new EnsureSortedStats(*this);
        return specific;
    }

    // The number of out-of-order results that were dropped.
    long long nDropped;
};

struct FetchStats : public SpecificStats {
    FetchStats() : alreadyHasObj(0), forcedFetches(0), docsExamined(0) {}

    SpecificStats* clone() const final {
        FetchStats* specific = new FetchStats(*this);
        return specific;
    }

    // Have we seen anything that already had an object?
    size_t alreadyHasObj; //赋值见FetchStage::doWork

    // How many records were we forced to fetch as the result of an invalidation?
    size_t forcedFetches;

    // The total number of full documents touched by the fetch stage.
    //size_t docsExamined; FetchStage::returnIfMatches中自增     keysExamined在IndexScan::doWork自增
    size_t docsExamined; //FetchStage::returnIfMatches中自增
};

struct GroupStats : public SpecificStats {
    GroupStats() : nGroups(0) {}

    SpecificStats* clone() const final {
        GroupStats* specific = new GroupStats(*this);
        return specific;
    }

    // The total number of groups.
    size_t nGroups;
};

struct IDHackStats : public SpecificStats {
    IDHackStats() : keysExamined(0), docsExamined(0) {}

    SpecificStats* clone() const final {
        IDHackStats* specific = new IDHackStats(*this);
        return specific;
    }

    std::string indexName;

    // Number of entries retrieved from the index while executing the idhack.
    size_t keysExamined;

    // Number of documents retrieved from the collection while executing the idhack.
    size_t docsExamined;
};

//index scan 统计信息  //IndexScan._specificStats为该成员
struct IndexScanStats : public SpecificStats {
    IndexScanStats()
        : indexVersion(0),
          direction(1),
          isMultiKey(false),
          isPartial(false),
          isSparse(false),
          isUnique(false),
          dupsTested(0),
          dupsDropped(0),
          seenInvalidated(0),
          keysExamined(0),
          seeks(0) {}

    SpecificStats* clone() const final {
        IndexScanStats* specific = new IndexScanStats(*this);
        // BSON objects have to be explicitly copied.
        specific->keyPattern = keyPattern.getOwned();
        specific->collation = collation.getOwned();
        specific->indexBounds = indexBounds.getOwned();
        return specific;
    }

    // Index type being used.
    std::string indexType;

    // name of the index being used
    std::string indexName;

    BSONObj keyPattern;

    BSONObj collation;

    int indexVersion;

    // A BSON (opaque, ie. hands off other than toString() it) representation of the bounds
    // used.
    BSONObj indexBounds;

    // >1 if we're traversing the index along with its order. <1 if we're traversing it
    // against the order.
    int direction;

    // index properties
    // Whether this index is over a field that contain array values.
    bool isMultiKey;

    // Represents which prefixes of the indexed field(s) cause the index to be multikey.
    MultikeyPaths multiKeyPaths;

    bool isPartial;
    bool isSparse;
    bool isUnique;

    size_t dupsTested;
    size_t dupsDropped;

    size_t seenInvalidated;
    // TODO: we could track key sizes here.

    // Number of entries retrieved from the index during the scan.
    //size_t docsExamined; FetchStage::returnIfMatches中自增     keysExamined在IndexScan::doWork自增
    size_t keysExamined;  //IndexScan::doWork赋值

    // Number of times the index cursor is re-positioned during the execution of the scan.
    size_t seeks; //IndexScan::doWork赋值
};

struct LimitStats : public SpecificStats {
    LimitStats() : limit(0) {}

    SpecificStats* clone() const final {
        LimitStats* specific = new LimitStats(*this);
        return specific;
    }

    size_t limit;
};

struct MockStats : public SpecificStats {
    MockStats() {}

    SpecificStats* clone() const final {
        return new MockStats(*this);
    }
};

//MultiPlanStage._specificStats  
struct MultiPlanStats : public SpecificStats {
    MultiPlanStats() {}

    SpecificStats* clone() const final {
        return new MultiPlanStats(*this);
    }
};

struct OrStats : public SpecificStats {
    OrStats() : dupsTested(0), dupsDropped(0), recordIdsForgotten(0) {}

    SpecificStats* clone() const final {
        OrStats* specific = new OrStats(*this);
        return specific;
    }

    size_t dupsTested;
    size_t dupsDropped;

    // How many calls to invalidate(...) actually removed a RecordId from our deduping map?
    size_t recordIdsForgotten;
};

struct ProjectionStats : public SpecificStats {
    ProjectionStats() {}

    SpecificStats* clone() const final {
        ProjectionStats* specific = new ProjectionStats(*this);
        return specific;
    }

    // Object specifying the projection transformation to apply.
    BSONObj projObj;
};

struct SortStats : public SpecificStats {
    SortStats() : forcedFetches(0), memUsage(0), memLimit(0) {}

    SpecificStats* clone() const final {
        SortStats* specific = new SortStats(*this);
        return specific;
    }

    // How many records were we forced to fetch as the result of an invalidation?
    size_t forcedFetches;

    // What's our current memory usage?
    size_t memUsage;

    // What's our memory limit?
    size_t memLimit;

    // The number of results to return from the sort.
    size_t limit;

    // The pattern according to which we are sorting.
    BSONObj sortPattern;
};

struct MergeSortStats : public SpecificStats {
    MergeSortStats() : dupsTested(0), dupsDropped(0), forcedFetches(0) {}

    SpecificStats* clone() const final {
        MergeSortStats* specific = new MergeSortStats(*this);
        return specific;
    }

    size_t dupsTested;
    size_t dupsDropped;

    // How many records were we forced to fetch as the result of an invalidation?
    size_t forcedFetches;

    // The pattern according to which we are sorting.
    BSONObj sortPattern;
};

struct ShardingFilterStats : public SpecificStats {
    ShardingFilterStats() : chunkSkips(0) {}

    SpecificStats* clone() const final {
        ShardingFilterStats* specific = new ShardingFilterStats(*this);
        return specific;
    }

    size_t chunkSkips;
};

struct SkipStats : public SpecificStats {
    SkipStats() : skip(0) {}

    SpecificStats* clone() const final {
        SkipStats* specific = new SkipStats(*this);
        return specific;
    }

    size_t skip;
};

struct IntervalStats {
    // Number of results found in the covering of this interval.
    long long numResultsBuffered = 0;
    // Number of documents in this interval returned to the parent stage.
    long long numResultsReturned = 0;

    // Min distance of this interval - always inclusive.
    double minDistanceAllowed = -1;
    // Max distance of this interval - inclusive iff inclusiveMaxDistanceAllowed.
    double maxDistanceAllowed = -1;
    // True only in the last interval.
    bool inclusiveMaxDistanceAllowed = false;
};

struct NearStats : public SpecificStats {
    NearStats() : indexVersion(0) {}

    SpecificStats* clone() const final {
        return new NearStats(*this);
    }

    std::vector<IntervalStats> intervalStats;
    std::string indexName;
    // btree index version, not geo index version
    int indexVersion;
    BSONObj keyPattern;
};

struct UpdateStats : public SpecificStats {
    UpdateStats()
        : nMatched(0),
          nModified(0),
          isDocReplacement(false),
          fastmodinsert(false),
          inserted(false),
          nInvalidateSkips(0) {}

    SpecificStats* clone() const final {
        return new UpdateStats(*this);
    }

    // The number of documents which match the query part of the update.
    size_t nMatched;

    // The number of documents modified by this update.
    size_t nModified;

    // True iff this is a doc-replacement style update, as opposed to a $mod update.
    bool isDocReplacement;

    // A 'fastmodinsert' is an insert resulting from an {upsert: true} update
    // which is a doc-replacement style update. It's "fast" because we don't need
    // to compute the document to insert based on the modifiers.
    bool fastmodinsert;

    // Is this an {upsert: true} update that did an insert?
    bool inserted;

    // The object that was inserted. This is an empty document if no insert was performed.
    BSONObj objInserted;

    // Invalidated documents can be force-fetched, causing the now invalid RecordId to
    // be thrown out. The update stage skips over any results which do not have the
    // RecordId to update.
    size_t nInvalidateSkips;
};

struct TextStats : public SpecificStats {
    TextStats() : parsedTextQuery(), textIndexVersion(0) {}

    SpecificStats* clone() const final {
        TextStats* specific = new TextStats(*this);
        return specific;
    }

    std::string indexName;

    // Human-readable form of the FTSQuery associated with the text stage.
    BSONObj parsedTextQuery;

    int textIndexVersion;

    // Index keys that precede the "text" index key.
    BSONObj indexPrefix;
};

struct TextMatchStats : public SpecificStats {
    TextMatchStats() : docsRejected(0) {}

    SpecificStats* clone() const final {
        TextMatchStats* specific = new TextMatchStats(*this);
        return specific;
    }

    size_t docsRejected;
};

struct TextOrStats : public SpecificStats {
    TextOrStats() : fetches(0) {}

    SpecificStats* clone() const final {
        TextOrStats* specific = new TextOrStats(*this);
        return specific;
    }

    size_t fetches;
};

}  // namespace mongo
