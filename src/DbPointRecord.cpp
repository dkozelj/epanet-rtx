//
//  DbPointRecord.cpp
//  epanet-rtx
//
//  Open Water Analytics [wateranalytics.org]
//  See README.md and license.txt for more information
//


#include <iostream>
#include <sstream>
#include <set>
#include <vector>
#include <boost/foreach.hpp>
#include <boost/algorithm/string/join.hpp>
#include <string>


#include "DbPointRecord.h"


using namespace RTX;
using namespace std;


std::string DbPointRecord::Query::selectStr() {
  stringstream ss;
  ss << "SELECT ";
  if (this->select.size() == 0) {
    ss << "*";
  }
  else {
    ss << boost::algorithm::join(this->select,", ");
  }
  
  ss << " FROM " << this->nameAndWhereClause();
  
  if (this->order.length() > 0) {
    ss << " ORDER BY " << this->order;
  }
  
  return ss.str();
}

std::string DbPointRecord::Query::nameAndWhereClause() {
  stringstream ss;
  ss << this->from;
  
  if (this->where.size() > 0) {
    ss << " WHERE " << boost::algorithm::join(this->where," AND ");
  }
  return ss.str();
}

DbPointRecord::request_t::request_t(string id, time_t start, time_t end) : id(id), range(make_pair(start, end)) {
  
}

bool DbPointRecord::request_t::contains(std::string id, time_t t) {
  if (this->range.first <= t && t <= this->range.second && RTX_STRINGS_ARE_EQUAL(id, this->id)) {
    return true;
  }
  return false;
}



DbPointRecord::DbPointRecord() : request("",0,0) {
  _searchDistance = 60*60*24*7; // 1-week
  errorMessage = "Not Connected";
  _readOnly = false;
  _filterType = OpcPassThrough;
  _identifiersAndUnitsCache = std::map<std::string,Units>();
  
  iterativeSearchMaxIterations = 8;
  iterativeSearchStride = 3*60*60;
  
  
  useTransactions = false;
  maxTransactionInserts = 1000;
  transactionInsertCount = 0;

}



bool DbPointRecord::readonly() {
  return _readOnly;
}

void DbPointRecord::setReadonly(bool readOnly) {
  _readOnly = readOnly;
}

bool DbPointRecord::registerAndGetIdentifierForSeriesWithUnits(string name, Units units) {
  
  if (name.length() == 0) {
    return false;
  }
  
  bool nameExists = false;
  bool unitsMatch = false;
  Units existingUnits = RTX_NO_UNITS;
  
  if (!this->isConnected()) {
    this->dbConnect();
  }
  
  const std::map<std::string,Units>& existing = this->identifiersAndUnits();
  std::map<string,Units>::const_iterator found = existing.find(name);
  if (found != existing.end()) {
    nameExists = true;
    existingUnits = found->second;
    if (existingUnits == units) {
      unitsMatch = true;
    }
  }
  
  if (this->readonly()) {
    // handle a read-only database.
    if (nameExists && (unitsMatch || !this->supportsUnitsColumn()) ) {
      // everything is awesome. name matches, units match (or we don't support it and therefore don't care).
      // make a cache and return affirmative.
      DB_PR_SUPER::registerAndGetIdentifierForSeriesWithUnits(name, units);
      return true;
    }
    else {
      // SPECIAL CASE FOR OLD RECORDS: we can update the units field if no units are specified.
      if (this->canAssignUnits() && existingUnits == RTX_NO_UNITS) {
        this->assignUnitsToRecord(name, units);
        DB_PR_SUPER::registerAndGetIdentifierForSeriesWithUnits(name, units);
        return true;
      }
      // names don't match (or units prevent us from using this record) and we can't write to this db. fail.
      return false;
    }
  }
  else {
    // not a readonly db.
    if (nameExists && !unitsMatch) {
      // two possibilities: the units actually don't match, or my units haven't ever been set.
      if (existingUnits == RTX_NO_UNITS) {
        if (this->canAssignUnits()) {
          // aha. update my units then.
          return this->assignUnitsToRecord(name, units);
        }
      }
      else {
        // must remove the old record. units don't match for real.
        this->removeRecord(name);
        return DB_PR_SUPER::registerAndGetIdentifierForSeriesWithUnits(name, units);
      }
    }
    else if ( ( !nameExists || !unitsMatch ) && this->insertIdentifierAndUnits(name, units) && DB_PR_SUPER::registerAndGetIdentifierForSeriesWithUnits(name, units) ) {
      // this will either insert a new record name, or ignore because it's already there.
      return true;
    }
    else if (nameExists && unitsMatch) {
      return DB_PR_SUPER::registerAndGetIdentifierForSeriesWithUnits(name, units);
    }
  }
  return false;
}

bool DbPointRecord::canAssignUnits() {
  return false;
}

bool DbPointRecord::assignUnitsToRecord(const std::string& name, const Units& units) {
  // nothing
  return false;
}

const std::map<std::string,Units> DbPointRecord::identifiersAndUnits() {
  return _identifiersAndUnitsCache;
}


void DbPointRecord::setSearchDistance(time_t time) {
  _searchDistance = time;
}

time_t DbPointRecord::searchDistance() {
  return _searchDistance;
}


Point DbPointRecord::point(const string& id, time_t time) {
  
  Point p = DB_PR_SUPER::point(id, time);
  
  if (!p.isValid) {
    
    // see if we just asked the db for something in this range.
    // if so, and Super couldn't find it, then it's just not here.
    // todo -- check staleness
    
    if (request.contains(id, time)) {
      return Point();
    }
    
    time_t margin = 60*60*12;
    time_t start = time - margin, end = time + margin;
    
    // do the request, and cache the request parameters.
    
    vector<Point> pVec = this->selectRange(id, start, end);
    pVec = this->pointsWithOpcFilter(pVec);
    
    if (pVec.size() > 0) {
      request = request_t(id, pVec.front().time, pVec.back().time);
    }
    else {
      request = request_t(id,0,0);
    }
    
    
    vector<Point>::const_iterator pIt = pVec.begin();
    while (pIt != pVec.end()) {
      if (pIt->time == time) {
        p = *pIt;
      }
      else if (pIt->time > time) {
        break;
      }
      ++pIt;
    }
    // cache this latest result set
    DB_PR_SUPER::addPoints(id, pVec);
  }
  
  
  
  return p;
}


Point DbPointRecord::pointBefore(const string& id, time_t time) {
  
  // available in circular buffer?
  Point p = DB_PR_SUPER::pointBefore(id, time);
  if (p.isValid) {
    return p;
  }
  
  // if it's not buffered, but the last request covered this range, then there is no point here.
  if (request.contains(id, time-1)) {
    return Point();
  }
  
  // should i search iteratively?
  if (this->shouldSearchIteratively()) {
    p = this->searchPreviousIteratively(id, time);
  }
  if (p.isValid) {
    return p;
  }
  
  // try a singly-bounded query
  if (this->supportsSinglyBoundedQueries()) {
    p = this->selectPrevious(id, time);
  }
  if (p.isValid) {
    return this->pointWithOpcFilter(p);
  }

  
  return p;
}


Point DbPointRecord::pointAfter(const string& id, time_t time) {
  // buffered?
  Point p = DB_PR_SUPER::pointAfter(id, time);
  if (p.isValid) {
    return p;
  }
  
  // last request covered this already?
  if (request.contains(id, time + 1)) {
    return Point();
  }
  
  if (this->shouldSearchIteratively()) {
    p = this->searchNextIteratively(id, time);
  }
  if (p.isValid) {
    return p;
  }
  
  // singly bounded?
  if (this->supportsSinglyBoundedQueries()) {
    p = this->selectNext(id, time);
  }
  if (p.isValid) {
    return this->pointWithOpcFilter(p);
  }
  
  return p;
}


Point DbPointRecord::searchPreviousIteratively(const string& id, time_t time) {
  int lookBehindLimit = iterativeSearchMaxIterations;
  
  if (!isConnected()) {
    this->dbConnect();
  }
  if (isConnected()) {
    vector<Point> points;
    // iterative lookbehind is faster than unbounded lookup
    time_t searchStartTime = time - iterativeSearchStride;
    time_t searchEndTime = time - 1;
    while (points.size() == 0 && lookBehindLimit > 0) {
      points = this->pointsInRange(id, searchStartTime, searchEndTime);
      searchEndTime -= iterativeSearchStride;
      searchStartTime -= iterativeSearchStride;
      --lookBehindLimit;
    }
    if (points.size() > 0) {
      return points.back();
    }
  }
  return Point();
}

Point DbPointRecord::searchNextIteratively(const string& id, time_t time) {
  int lookAheadLimit = iterativeSearchMaxIterations;
  
  if (!isConnected()) {
    this->dbConnect();
  }
  if (isConnected()) {
    vector<Point> points;
    // iterative lookbehind is faster than unbounded lookup
    time_t searchStartTime = time + 1;
    time_t searchEndTime = time + iterativeSearchStride;
    while (points.size() == 0 && lookAheadLimit > 0) {
      points = this->pointsInRange(id, searchStartTime, searchEndTime);
      searchStartTime += iterativeSearchStride;
      searchEndTime += iterativeSearchStride;
      --lookAheadLimit;
    }
    if (points.size() > 0) {
      return points.front();
    }
  }
  return Point();
}


std::vector<Point> DbPointRecord::pointsInRange(const string& id, time_t startTime, time_t endTime) {
  
  PointRecord::time_pair_t range = DB_PR_SUPER::range(id);
  
  // if the requested range is not in memcache, then fetch it.
  if ( !(range.first <= startTime && endTime <= range.second) ) {
    vector<Point> left;
    vector<Point> right;
    
    time_t qstart, qend;
    if (startTime < range.first && endTime < range.second  && endTime > range.first) {
      // left-fill query
      qstart = startTime;
      qend = range.first;
      right = DB_PR_SUPER::pointsInRange(id, range.first, endTime);
    }
    else if (range.first < startTime && range.second < endTime && startTime < range.second ) {
      // right-fill query
      qstart = range.second;
      qend = endTime;
      left = DB_PR_SUPER::pointsInRange(id, startTime, range.second);
    }
    else {
      // full overlap -- todo - generate two queries...
      qstart = startTime;
      qend = endTime;
    }
    // db hit
    vector<Point> newPoints = this->selectRange(id, qstart, qend);
    newPoints = this->pointsWithOpcFilter(newPoints);
    
    vector<Point> merged;
    merged.reserve(newPoints.size() + left.size() + right.size());
    merged.insert(merged.end(), left.begin(), left.end());
    merged.insert(merged.end(), newPoints.begin(), newPoints.end());
    merged.insert(merged.end(), right.begin(), right.end());

    set<time_t> addedTimes;
    vector<Point> deDuped;
    deDuped.reserve(merged.size());
    BOOST_FOREACH(const Point& p, merged) {
      if (addedTimes.count(p.time) == 0) {
        addedTimes.insert(p.time);
        if (startTime <= p.time && p.time <= endTime) {
          deDuped.push_back(p);
        }
      }
    }
    
    request = (deDuped.size() > 0) ? request_t(id, deDuped.front().time, deDuped.back().time) : request_t(id,0,0);
    
    DB_PR_SUPER::addPoints(id, deDuped);
    
    return deDuped;
  }
  
  return DB_PR_SUPER::pointsInRange(id, startTime, endTime);
  
}


void DbPointRecord::addPoint(const string& id, Point point) {
  if (!this->readonly()) {
    DB_PR_SUPER::addPoint(id, point);
    this->insertSingle(id, point);
  }
}


void DbPointRecord::addPoints(const string& id, std::vector<Point> points) {
  if (!this->readonly()) {
    DB_PR_SUPER::addPoints(id, points);
    this->insertRange(id, points);
  }
}


void DbPointRecord::reset() {
  if (!this->readonly()) {
    DB_PR_SUPER::reset();
    //this->truncate();
  }
}


void DbPointRecord::reset(const string& id) {
  if (!this->readonly()) {
    // deprecate?
    //cout << "Whoops - don't use this" << endl;
    DB_PR_SUPER::reset(id);
    //this->removeRecord(id);
    // wiped out the record completely, so re-initialize it.
    //this->registerAndGetIdentifier(id);
  }
}

void DbPointRecord::invalidate(const string &identifier) {
  if (!this->readonly()) {
    this->removeRecord(identifier);
    this->reset(identifier);
  }
}


#pragma mark - opc filter list

void DbPointRecord::setOpcFilterType(OpcFilterType type) {
  if (_filterType != type) {
    BufferPointRecord::reset(); // mem cache
    _filterType = type;
    this->dbConnect();
  }
}

DbPointRecord::OpcFilterType DbPointRecord::opcFilterType() {
  return _filterType;
}

std::set<unsigned int> DbPointRecord::opcFilterList() {
  return _opcFilterCodes;
}

void DbPointRecord::clearOpcFilterList() {
  _opcFilterCodes.clear();
  BufferPointRecord::reset(); // mem cache
  this->dbConnect();
}

void DbPointRecord::addOpcFilterCode(unsigned int code) {
  _opcFilterCodes.insert(code);
  BufferPointRecord::reset(); // mem cache
  this->dbConnect();
}

void DbPointRecord::removeOpcFilterCode(unsigned int code) {
  if (_opcFilterCodes.count(code) > 0) {
    _opcFilterCodes.erase(code);
    BufferPointRecord::reset(); // mem cache
    this->dbConnect();
  }
}



vector<Point> DbPointRecord::pointsWithOpcFilter(std::vector<Point> points) {
  vector<Point> out;
  
  BOOST_FOREACH(Point p, points) {
    Point outPoint = this->pointWithOpcFilter(p);
    if (outPoint.isValid) {
      out.push_back(outPoint);
    }
  }
  
  return out;
}

Point DbPointRecord::pointWithOpcFilter(Point p) {
  Point pOut = p;
  
  switch (this->opcFilterType()) {
    case OpcPassThrough:
      break;
    case OpcWhiteList:
      if (this->opcFilterList().count(p.quality) > 0) {
        pOut = Point(p.time, p.value, Point::opc_rtx_override, p.confidence);
      }
      else {
        pOut = Point();
      }
      break;
    case OpcBlackList:
      if (this->opcFilterList().count(p.quality)) {
        pOut = Point();
      }
      else {
        pOut = Point(p.time, p.value, Point::opc_rtx_override, p.confidence);
      }
      break;
    case OpcCodesToValues:
    {
      pOut = Point(p.time, (double)p.quality, Point::opc_rtx_override, p.confidence);
    }
      break;
    case OpcCodesToConfidence:
    {
      pOut = Point(p.time, p.value, Point::opc_rtx_override, (double)p.quality);
    }
      break;
    default:
      break;
  }
  
  return pOut;
  
}



/*
Point DbPointRecord::firstPoint(const string& id) {
  
}


Point DbPointRecord::lastPoint(const string& id) {
  
}

*/





/*
// default virtual implementations
void DbPointRecord::fetchRange(const std::string& id, time_t startTime, time_t endTime) {
  reqRange = make_pair(startTime, endTime);
  
  vector<Point> points = selectRange(id, startTime, endTime);
  if (points.size() > 0) {
    DB_PR_SUPER::addPoints(id, points);
  }
}

void DbPointRecord::fetchNext(const std::string& id, time_t time) {
  Point p = selectNext(id, time);
  if (p.isValid) {
    DB_PR_SUPER::addPoint(id, p);
  }
}


void DbPointRecord::fetchPrevious(const std::string& id, time_t time) {
  Point p = selectPrevious(id, time);
  if (p.isValid) {
    DB_PR_SUPER::addPoint(id, p);
  }
}
*/






/*





void DbPointRecord::preFetchRange(const string& id, time_t start, time_t end) {
  // TODO -- performance optimization - caching -- see OdbcPointRecord.cpp for code snippets.
  // get out if we've already hinted this.

  time_t first = DB_PR_SUPER::firstPoint(id).time;
  time_t last = DB_PR_SUPER::lastPoint(id).time;
  if (first <= start && end <= last) {
    return;
  }
  
  // clear out the base-class cache
  //PointRecord::reset(id);
  
  // re-populate base class with new hinted range
  time_t margin = 60*60;
  
  cout << "RTX-DB-FETCH: " << id << " :: " << start << " - " << end << endl;
  
  vector<Point> newPoints = selectRange(id, start - margin, end + margin);

  //cout << "RTX-DB-FETCH: " << id << " :: DONE" << endl;
  
  DB_PR_SUPER::addPoints(id, newPoints);
}

*/