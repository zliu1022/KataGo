
//-------------------------------------------------------------------------------------
//This file contains the main core logic of the search.
//-------------------------------------------------------------------------------------

#include "../search/search.h"

#include <algorithm>

#include "../core/fancymath.h"
#include "../core/timer.h"
#include "../search/distributiontable.h"

using namespace std;

ReportedSearchValues::ReportedSearchValues()
{}
ReportedSearchValues::~ReportedSearchValues()
{}

NodeStats::NodeStats()
  :visits(0),winValueSum(0.0),noResultValueSum(0.0),scoreMeanSum(0.0),scoreMeanSqSum(0.0),leadSum(0.0),utilitySum(0.0),utilitySqSum(0.0),weightSum(0.0),weightSqSum(0.0)
{}
NodeStats::~NodeStats()
{}

NodeStats::NodeStats(const NodeStats& other)
  :visits(other.visits),
   winValueSum(other.winValueSum),
   noResultValueSum(other.noResultValueSum),
   scoreMeanSum(other.scoreMeanSum),
   scoreMeanSqSum(other.scoreMeanSqSum),
   leadSum(other.leadSum),
   utilitySum(other.utilitySum),
   utilitySqSum(other.utilitySqSum),
   weightSum(other.weightSum),
   weightSqSum(other.weightSqSum)
{}
NodeStats& NodeStats::operator=(const NodeStats& other) {
  visits = other.visits;
  winValueSum = other.winValueSum;
  noResultValueSum = other.noResultValueSum;
  scoreMeanSum = other.scoreMeanSum;
  scoreMeanSqSum = other.scoreMeanSqSum;
  leadSum = other.leadSum;
  utilitySum = other.utilitySum;
  utilitySqSum = other.utilitySqSum;
  weightSum = other.weightSum;
  weightSqSum = other.weightSqSum;
  return *this;
}

double NodeStats::getResultUtilitySum(const SearchParams& searchParams) const {
  return (
    (2.0*winValueSum - weightSum + noResultValueSum) * searchParams.winLossUtilityFactor +
    noResultValueSum * searchParams.noResultUtilityForWhite
  );
}

double Search::getResultUtility(double winValue, double noResultValue) const {
  return (
    (2.0*winValue - 1.0 + noResultValue) * searchParams.winLossUtilityFactor +
    noResultValue * searchParams.noResultUtilityForWhite
  );
}

double Search::getResultUtilityFromNN(const NNOutput& nnOutput) const {
  return (
    (nnOutput.whiteWinProb - nnOutput.whiteLossProb) * searchParams.winLossUtilityFactor +
    nnOutput.whiteNoResultProb * searchParams.noResultUtilityForWhite
  );
}

double Search::getScoreStdev(double scoreMean, double scoreMeanSq) {
  double variance = scoreMeanSq - scoreMean * scoreMean;
  if(variance <= 0.0)
    return 0.0;
  return sqrt(variance);
}

//-----------------------------------------------------------------------------------------

SearchNode::SearchNode(Search& search, SearchThread& thread, Loc moveLoc)
  :lockIdx(),nextPla(thread.pla),prevMoveLoc(moveLoc),
   nnOutput(),
   nnOutputAge(0),
   children(NULL),numChildren(0),childrenCapacity(0),
   stats(),virtualLosses(0)
{
  lockIdx = thread.rand.nextUInt(search.mutexPool->getNumMutexes());
}
SearchNode::~SearchNode() {
  if(children != NULL) {
    for(int i = 0; i<numChildren; i++)
      delete children[i];
  }
  delete[] children;
}

SearchNode::SearchNode(SearchNode&& other) noexcept
  :lockIdx(other.lockIdx),
   nextPla(other.nextPla),prevMoveLoc(other.prevMoveLoc),
   nnOutput(std::move(other.nnOutput)),
   nnOutputAge(other.nnOutputAge),
   stats(other.stats),virtualLosses(other.virtualLosses)
{
  children = other.children;
  other.children = NULL;
  numChildren = other.numChildren;
  childrenCapacity = other.childrenCapacity;
}
SearchNode& SearchNode::operator=(SearchNode&& other) noexcept {
  lockIdx = other.lockIdx;
  nextPla = other.nextPla;
  prevMoveLoc = other.prevMoveLoc;
  nnOutput = std::move(other.nnOutput);
  nnOutputAge = other.nnOutputAge;
  children = other.children;
  other.children = NULL;
  numChildren = other.numChildren;
  childrenCapacity = other.childrenCapacity;
  stats = other.stats;
  virtualLosses = other.virtualLosses;
  return *this;
}

//-----------------------------------------------------------------------------------------

static string makeSeed(const Search& search, int threadIdx) {
  stringstream ss;
  ss << search.randSeed;
  ss << "$searchThread$";
  ss << threadIdx;
  ss << "$";
  ss << search.rootBoard.pos_hash;
  ss << "$";
  ss << search.rootHistory.moveHistory.size();
  ss << "$";
  ss << search.numSearchesBegun;
  return ss.str();
}

SearchThread::SearchThread(int tIdx, const Search& search, Logger* lg)
  :threadIdx(tIdx),
   pla(search.rootPla),board(search.rootBoard),
   history(search.rootHistory),
   rand(makeSeed(search,tIdx)),
   nnResultBuf(),
   logStream(NULL),
   logger(lg),
   weightFactorBuf(),
   weightBuf(),
   weightSqBuf(),
   winValuesBuf(),
   noResultValuesBuf(),
   scoreMeansBuf(),
   scoreMeanSqsBuf(),
   leadsBuf(),
   utilityBuf(),
   utilitySqBuf(),
   selfUtilityBuf(),
   visitsBuf()
{
  if(logger != NULL)
    logStream = logger->createOStream();

  weightFactorBuf.reserve(NNPos::MAX_NN_POLICY_SIZE);

  weightBuf.resize(NNPos::MAX_NN_POLICY_SIZE);
  weightSqBuf.resize(NNPos::MAX_NN_POLICY_SIZE);
  winValuesBuf.resize(NNPos::MAX_NN_POLICY_SIZE);
  noResultValuesBuf.resize(NNPos::MAX_NN_POLICY_SIZE);
  scoreMeansBuf.resize(NNPos::MAX_NN_POLICY_SIZE);
  scoreMeanSqsBuf.resize(NNPos::MAX_NN_POLICY_SIZE);
  leadsBuf.resize(NNPos::MAX_NN_POLICY_SIZE);
  utilityBuf.resize(NNPos::MAX_NN_POLICY_SIZE);
  utilitySqBuf.resize(NNPos::MAX_NN_POLICY_SIZE);
  selfUtilityBuf.resize(NNPos::MAX_NN_POLICY_SIZE);
  visitsBuf.resize(NNPos::MAX_NN_POLICY_SIZE);

}
SearchThread::~SearchThread() {
  if(logStream != NULL)
    delete logStream;
  logStream = NULL;
  logger = NULL;
}

//-----------------------------------------------------------------------------------------

static const double VALUE_WEIGHT_DEGREES_OF_FREEDOM = 3.0;

Search::Search(SearchParams params, NNEvaluator* nnEval, const string& rSeed)
  :rootPla(P_BLACK),rootBoard(),rootHistory(),rootPassLegal(true),
   rootSafeArea(NULL),
   recentScoreCenter(0.0),
   alwaysIncludeOwnerMap(false),
   searchParams(params),numSearchesBegun(0),searchNodeAge(0),
   rootPlaDuringLastSearch(C_EMPTY),
   randSeed(rSeed),
   normToTApproxZ(0.0),
   nnEvaluator(nnEval),
   nonSearchRand(rSeed + string("$nonSearchRand"))
{
  nnXLen = nnEval->getNNXLen();
  nnYLen = nnEval->getNNYLen();
  assert(nnXLen > 0 && nnXLen <= NNPos::MAX_BOARD_LEN);
  assert(nnYLen > 0 && nnYLen <= NNPos::MAX_BOARD_LEN);
  policySize = NNPos::getPolicySize(nnXLen,nnYLen);
  rootKoHashTable = new KoHashTable();

  rootSafeArea = new Color[Board::MAX_ARR_SIZE];

  valueWeightDistribution = new DistributionTable(
    [](double z) { return FancyMath::tdistpdf(z,VALUE_WEIGHT_DEGREES_OF_FREEDOM); },
    [](double z) { return FancyMath::tdistcdf(z,VALUE_WEIGHT_DEGREES_OF_FREEDOM); },
    -50.0,
    50.0,
    2000
  );

  rootNode = NULL;
  mutexPool = new MutexPool(params.mutexPoolSize);

  rootHistory.clear(rootBoard,rootPla,Rules(),0);
  rootKoHashTable->recompute(rootHistory);
}

Search::~Search() {
  delete[] rootSafeArea;
  delete rootKoHashTable;
  delete valueWeightDistribution;
  delete rootNode;
  delete mutexPool;
}

const Board& Search::getRootBoard() const {
  return rootBoard;
}
const BoardHistory& Search::getRootHist() const {
  return rootHistory;
}
Player Search::getRootPla() const {
  return rootPla;
}

void Search::setPosition(Player pla, const Board& board, const BoardHistory& history) {
  clearSearch();
  rootPla = pla;
  rootBoard = board;
  rootHistory = history;
  rootKoHashTable->recompute(rootHistory);
}

void Search::setPlayerAndClearHistory(Player pla) {
  clearSearch();
  rootPla = pla;
  rootBoard.clearSimpleKoLoc();
  Rules rules = rootHistory.rules;

  //Preserve this value even when we get multiple moves in a row by some player
  bool assumeMultipleStartingBlackMovesAreHandicap = rootHistory.assumeMultipleStartingBlackMovesAreHandicap;
  rootHistory.clear(rootBoard,rootPla,rules,rootHistory.encorePhase);
  rootHistory.setAssumeMultipleStartingBlackMovesAreHandicap(assumeMultipleStartingBlackMovesAreHandicap);

  rootKoHashTable->recompute(rootHistory);
}

void Search::setKomiIfNew(float newKomi) {
  if(rootHistory.rules.komi != newKomi) {
    clearSearch();
    rootHistory.setKomi(newKomi);
  }
}

void Search::setRootPassLegal(bool b) {
  clearSearch();
  rootPassLegal = b;
}

void Search::setAlwaysIncludeOwnerMap(bool b) {
  if(!alwaysIncludeOwnerMap && b)
    clearSearch();
  alwaysIncludeOwnerMap = b;
}

void Search::setParams(SearchParams params) {
  clearSearch();
  searchParams = params;
}

void Search::setParamsNoClearing(SearchParams params) {
  searchParams = params;
}

void Search::setNNEval(NNEvaluator* nnEval) {
  clearSearch();
  nnEvaluator = nnEval;
  nnXLen = nnEval->getNNXLen();
  nnYLen = nnEval->getNNYLen();
  assert(nnXLen > 0 && nnXLen <= NNPos::MAX_BOARD_LEN);
  assert(nnYLen > 0 && nnYLen <= NNPos::MAX_BOARD_LEN);
  policySize = NNPos::getPolicySize(nnXLen,nnYLen);
}

void Search::clearSearch() {
  delete rootNode;
  rootNode = NULL;
}

bool Search::isLegalTolerant(Loc moveLoc, Player movePla) const {
  //Tolerate sgf files or GTP reporting suicide moves, even if somehow the rules are set to disallow them.
  bool multiStoneSuicideLegal = true;

  //If we somehow have the same player making multiple moves in a row (possible in GTP or an sgf file),
  //clear the ko loc - the simple ko loc of a player should not prohibit the opponent playing there!
  if(movePla != rootPla) {
    Board copy = rootBoard;
    copy.clearSimpleKoLoc();
    return copy.isLegal(moveLoc,movePla,multiStoneSuicideLegal);
  }
  else {
    //Don't require that the move is legal for the history, merely the board, so that
    //we're robust to GTP or an sgf file saying that a move was made that violates superko or things like that.
    //In the encore, we also need to ignore the simple ko loc, since the board itself will report a move as illegal
    //when actually it is a legal pass-for-ko.
    if(rootHistory.encorePhase >= 1)
      return rootBoard.isLegalIgnoringKo(moveLoc,rootPla,multiStoneSuicideLegal);
    else
      return rootBoard.isLegal(moveLoc,rootPla,multiStoneSuicideLegal);
  }
}

bool Search::isLegalStrict(Loc moveLoc, Player movePla) const {
  return movePla == rootPla && rootHistory.isLegal(rootBoard,moveLoc,movePla);
}

bool Search::makeMove(Loc moveLoc, Player movePla) {
  return makeMove(moveLoc,movePla,false);
}

bool Search::makeMove(Loc moveLoc, Player movePla, bool preventEncore) {
  if(!isLegalTolerant(moveLoc,movePla))
    return false;

  if(movePla != rootPla)
    setPlayerAndClearHistory(movePla);

  if(rootNode != NULL) {
    bool foundChild = false;
    for(int i = 0; i<rootNode->numChildren; i++) {
      SearchNode* child = rootNode->children[i];
      if(child->prevMoveLoc == moveLoc) {
        //Grab out the node to prevent its deletion along with the root
        SearchNode* node = new SearchNode(std::move(*child));
        //Delete the root and replace it with the child
        delete rootNode;
        rootNode = node;
        rootNode->prevMoveLoc = Board::NULL_LOC;
        foundChild = true;
        break;
      }
    }
    if(!foundChild) {
      clearSearch();
    }
  }
  //If the white handicap bonus changes due to the move, we will also need to recompute everything since this is
  //basically like a change to the komi.
  float oldWhiteHandicapBonusScore = rootHistory.whiteHandicapBonusScore;

  rootHistory.makeBoardMoveAssumeLegal(rootBoard,moveLoc,rootPla,rootKoHashTable,preventEncore);
  rootPla = getOpp(rootPla);
  rootKoHashTable->recompute(rootHistory);

  if(rootHistory.whiteHandicapBonusScore != oldWhiteHandicapBonusScore)
    clearSearch();

  //TODO test this and other conservativepass
  //In the case that we are conservativePass and a pass would end the game, need to clear the search.
  //This is because deeper in the tree, such a node would have been explored as ending the game, but now that
  //it's a root pass, it needs to be treated as if it no longer ends the game.
  //In the case that we're preventing encore, and the phase would have ended, we also need to clear the search
  //since the search was conducted on the assumption that we're going into encore now.
  if((searchParams.conservativePass && rootHistory.passWouldEndGame(rootBoard,rootPla)) ||
     (preventEncore && rootHistory.passWouldEndPhase(rootBoard,rootPla)))
    clearSearch();

  return true;
}


double Search::getScoreUtility(double scoreMeanSum, double scoreMeanSqSum, double weightSum) const {
  double scoreMean = scoreMeanSum / weightSum;
  double scoreMeanSq = scoreMeanSqSum / weightSum;
  double scoreStdev = getScoreStdev(scoreMean, scoreMeanSq);
  double staticScoreValue = ScoreValue::expectedWhiteScoreValue(scoreMean,scoreStdev,0.0,2.0,rootBoard);
  double dynamicScoreValue = ScoreValue::expectedWhiteScoreValue(scoreMean,scoreStdev,recentScoreCenter,searchParams.dynamicScoreCenterScale,rootBoard);
  return staticScoreValue * searchParams.staticScoreUtilityFactor + dynamicScoreValue * searchParams.dynamicScoreUtilityFactor;
}

double Search::getScoreUtilityDiff(double scoreMeanSum, double scoreMeanSqSum, double weightSum, double delta) const {
  double scoreMean = scoreMeanSum / weightSum;
  double scoreMeanSq = scoreMeanSqSum / weightSum;
  double scoreStdev = getScoreStdev(scoreMean, scoreMeanSq);
  double staticScoreValueDiff =
    ScoreValue::expectedWhiteScoreValue(scoreMean + delta,scoreStdev,0.0,2.0,rootBoard)
    -ScoreValue::expectedWhiteScoreValue(scoreMean,scoreStdev,0.0,2.0,rootBoard);
  double dynamicScoreValueDiff =
    ScoreValue::expectedWhiteScoreValue(scoreMean + delta,scoreStdev,recentScoreCenter,searchParams.dynamicScoreCenterScale,rootBoard)
    -ScoreValue::expectedWhiteScoreValue(scoreMean,scoreStdev,recentScoreCenter,searchParams.dynamicScoreCenterScale,rootBoard);
  return staticScoreValueDiff * searchParams.staticScoreUtilityFactor + dynamicScoreValueDiff * searchParams.dynamicScoreUtilityFactor;
}

double Search::getUtilityFromNN(const NNOutput& nnOutput) const {
  double resultUtility = getResultUtilityFromNN(nnOutput);
  return resultUtility + getScoreUtility(nnOutput.whiteScoreMean, nnOutput.whiteScoreMeanSq, 1.0);
}

uint32_t Search::chooseIndexWithTemperature(Rand& rand, const double* relativeProbs, int numRelativeProbs, double temperature) {
  assert(numRelativeProbs > 0);
  assert(numRelativeProbs <= Board::MAX_ARR_SIZE); //We're just doing this on the stack
  double processedRelProbs[Board::MAX_ARR_SIZE];

  double maxValue = 0.0;
  for(int i = 0; i<numRelativeProbs; i++) {
    if(relativeProbs[i] > maxValue)
      maxValue = relativeProbs[i];
  }
  assert(maxValue > 0.0);

  //Temperature so close to 0 that we just calculate the max directly
  if(temperature <= 1.0e-4) {
    double bestProb = relativeProbs[0];
    int bestIdx = 0;
    for(int i = 1; i<numRelativeProbs; i++) {
      if(relativeProbs[i] > bestProb) {
        bestProb = relativeProbs[i];
        bestIdx = i;
      }
    }
    return bestIdx;
  }
  //Actual temperature
  else {
    double logMaxValue = log(maxValue);
    double sum = 0.0;
    for(int i = 0; i<numRelativeProbs; i++) {
      //Numerically stable way to raise to power and normalize
      processedRelProbs[i] = relativeProbs[i] <= 0.0 ? 0.0 : exp((log(relativeProbs[i]) - logMaxValue) / temperature);
      sum += processedRelProbs[i];
    }
    assert(sum > 0.0);
    uint32_t idxChosen = rand.nextUInt(processedRelProbs,numRelativeProbs);
    return idxChosen;
  }
}

double Search::interpolateEarly(double halflife, double earlyValue, double value) const {
  double rawHalflives = (rootHistory.initialTurnNumber + rootHistory.moveHistory.size()) / halflife;
  double halflives = rawHalflives * 19.0 / sqrt(rootBoard.x_size*rootBoard.y_size);
  return value + (earlyValue - value) * pow(0.5, halflives);
}

Loc Search::runWholeSearchAndGetMove(Player movePla, Logger& logger) {
  return runWholeSearchAndGetMove(movePla,logger,false);
}

Loc Search::runWholeSearchAndGetMove(Player movePla, Logger& logger, bool pondering) {
  runWholeSearch(movePla,logger,pondering);
  return getChosenMoveLoc();
}

void Search::runWholeSearch(Player movePla, Logger& logger) {
  runWholeSearch(movePla,logger,false);
}

void Search::runWholeSearch(Player movePla, Logger& logger, bool pondering) {
  if(movePla != rootPla)
    setPlayerAndClearHistory(movePla);
  std::atomic<bool> shouldStopNow(false);
  runWholeSearch(logger,shouldStopNow,pondering);
}

void Search::runWholeSearch(Logger& logger, std::atomic<bool>& shouldStopNow) {
  runWholeSearch(logger,shouldStopNow, false);
}

void Search::runWholeSearch(Logger& logger, std::atomic<bool>& shouldStopNow, bool pondering) {
  runWholeSearch(logger,shouldStopNow,pondering,TimeControls(),1.0);
}

void Search::runWholeSearch(Logger& logger, std::atomic<bool>& shouldStopNow, bool pondering, const TimeControls& tc, double searchFactor) {

  ClockTimer timer;
  atomic<int64_t> numPlayoutsShared(0);

  if(!std::atomic_is_lock_free(&numPlayoutsShared))
    logger.write("Warning: int64_t atomic numPlayoutsShared is not lock free");
  if(!std::atomic_is_lock_free(&shouldStopNow))
    logger.write("Warning: bool atomic shouldStopNow is not lock free");

  //Compute caps on search
  int64_t maxVisits = pondering ? searchParams.maxVisitsPondering : searchParams.maxVisits;
  int64_t maxPlayouts = pondering ? searchParams.maxPlayoutsPondering : searchParams.maxPlayouts;
  double_t maxTime = pondering ? searchParams.maxTimePondering : searchParams.maxTime;

  //Apply time controls
  {
    double tcMin;
    double tcRec;
    double tcMax;
    tc.getTime(rootBoard,rootHistory,searchParams.lagBuffer,tcMin,tcRec,tcMax);
    //Right now, just always use the recommended time.
    maxTime = std::min(tcRec,maxTime);
  }

  {
    //Possibly reduce computation time, for human friendliness
    if(rootHistory.moveHistory.size() >= 1 && rootHistory.moveHistory[rootHistory.moveHistory.size()-1].loc == Board::PASS_LOC) {
      if(rootHistory.moveHistory.size() >= 3 && rootHistory.moveHistory[rootHistory.moveHistory.size()-3].loc == Board::PASS_LOC)
        searchFactor *= searchParams.searchFactorAfterTwoPass;
      else
        searchFactor *= searchParams.searchFactorAfterOnePass;
    }

    if(searchFactor != 1.0) {
      double cap = (double)((int64_t)1L << 62);
      maxVisits = (int64_t)ceil(std::min(cap, maxVisits * searchFactor));
      maxPlayouts = (int64_t)ceil(std::min(cap, maxPlayouts * searchFactor));
      maxTime = maxTime * searchFactor;
    }
  }

  beginSearch(logger);
  int64_t numNonPlayoutVisits = numRootVisits();

  auto searchLoop = [this,&timer,&numPlayoutsShared,numNonPlayoutVisits,&logger,&shouldStopNow,maxVisits,maxPlayouts,maxTime](int threadIdx) {
    SearchThread* stbuf = new SearchThread(threadIdx,*this,&logger);

    int64_t numPlayouts = numPlayoutsShared.load(std::memory_order_relaxed);
    double_t curTime = 0;
    double_t lastTime = 0;
    try {
      while(true) {
        curTime = timer.getSeconds();
        if (threadIdx==0 && (curTime-lastTime)>=3){ // print log every 3 second, only in No.0 thread
          ReportedSearchValues values;
          values = this->getRootValuesAssertSuccess();
          int64_t visits = this->getRootVisits();
          double lead = values.lead;
          double winrate = 0.5 * (1.0 + (values.winValue - values.lossValue));
          if(this->rootPla == P_BLACK || (this->rootPla != P_BLACK && this->rootPla != P_WHITE && stbuf->pla == P_BLACK)) {
            winrate = 1.0 - winrate;
            lead = -lead;
          }

          vector<Loc> buf;
          vector<Loc> scratchLocs;
          vector<double> scratchValues;
          appendPV(buf,scratchLocs,scratchValues,this->rootNode,10);
          char pvbuf[10*4] = "";
          int maxpv = 10>buf.size()?buf.size():10;
          for(int i = 0; i<maxpv; i++) {
            if(buf[i] == Board::NULL_LOC)
              continue;
            sprintf(pvbuf,"%s %s",pvbuf, Location::toString(buf[i],this->rootBoard).c_str());
          }

          char b[256];
          sprintf(b,"V: %lld P: %lld, Win: %.2f%%, Score: %.1f(%.1f) PV: %s",visits, numPlayouts,winrate*100.0, lead, values.expectedScoreStdev,pvbuf);
          logger.write(b);
          lastTime = curTime;
        }
        bool shouldStop =
          (numPlayouts >= 2 && maxTime < 1.0e12 && timer.getSeconds() >= maxTime) ||
          (numPlayouts >= maxPlayouts) ||
          (numPlayouts + numNonPlayoutVisits >= maxVisits);

        if(shouldStop || shouldStopNow.load(std::memory_order_relaxed)) {
          shouldStopNow.store(true,std::memory_order_relaxed);
          break;
        }

        runSinglePlayout(*stbuf);

        numPlayouts = numPlayoutsShared.fetch_add((int64_t)1, std::memory_order_relaxed);
        numPlayouts += 1;
      }
    }
    catch(const exception& e) {
      logger.write(string("ERROR: Search thread failed: ") + e.what());
      delete stbuf;
      throw;
    }
    catch(const string& e) {
      logger.write("ERROR: Search thread failed: " + e);
      delete stbuf;
      throw;
    }
    catch(...) {
      logger.write("ERROR: Search thread failed with unexpected throw");
      delete stbuf;
      throw;
    }

    delete stbuf;
  };

  if(searchParams.numThreads <= 1)
    searchLoop(0);
  else {
    std::thread* threads = new std::thread[searchParams.numThreads-1];
    for(int i = 0; i<searchParams.numThreads-1; i++)
      threads[i] = std::thread(searchLoop,i+1);
    searchLoop(0);
    for(int i = 0; i<searchParams.numThreads-1; i++)
      threads[i].join();
    delete[] threads;
  }
}

//If we're being asked to search from a position where the game is over, this is fine. Just keep going, the boardhistory
//should reasonably tolerate just continuing. We do NOT want to clear history because we could inadvertently make a move
//that an external ruleset COULD think violated superko.
void Search::beginSearch(Logger& logger) {
  if(rootBoard.x_size > nnXLen || rootBoard.y_size > nnYLen)
    throw StringError("Search got from NNEval nnXLen = " + Global::intToString(nnXLen) +
                      " nnYLen = " + Global::intToString(nnYLen) + " but was asked to search board with larger x or y size");

  rootBoard.checkConsistency();

  numSearchesBegun++;
  searchNodeAge++;
  if(searchNodeAge == 0) //Just in case, as we roll over
    clearSearch();
  //In the case we are doing playoutDoublingAdvantage without a specific player (so, doing the root player)
  //and the root player changes, we need to clear the tree since we need new evals for the new way around
  if(rootPlaDuringLastSearch != rootPla && searchParams.playoutDoublingAdvantage != 0 && searchParams.playoutDoublingAdvantagePla == C_EMPTY)
    clearSearch();
  rootPlaDuringLastSearch = rootPla;

  computeRootValues(logger);
  maybeRecomputeNormToTApproxTable();

  //Sanity-check a few things
  if(!rootPassLegal && searchParams.rootPruneUselessMoves)
    throw StringError("Both rootPassLegal=false and searchParams.rootPruneUselessMoves=true are specified, this could leave the bot without legal moves!");

  SearchThread dummyThread(-1, *this, NULL);

  if(rootNode == NULL) {
    rootNode = new SearchNode(*this, dummyThread, Board::NULL_LOC);
  }
  else {
    //If the root node has any existing children, then prune things down if there are moves that should not be allowed at the root.
    SearchNode& node = *rootNode;
    int numChildren = node.numChildren;
    if(node.children != NULL && numChildren > 0) {
      assert(node.nnOutput != NULL);

      //Perform the filtering
      int numGoodChildren = 0;
      for(int i = 0; i<numChildren; i++) {
        SearchNode* child = node.children[i];
        node.children[i] = NULL;
        if(isAllowedRootMove(child->prevMoveLoc))
          node.children[numGoodChildren++] = child;
        else {
          delete child;
        }
      }
      bool anyFiltered = numChildren != numGoodChildren;
      node.numChildren = numGoodChildren;
      numChildren = numGoodChildren;

      if(anyFiltered) {
        //Fix up the number of visits of the root node after doing this filtering
        int64_t newNumVisits = 0;
        for(int i = 0; i<numChildren; i++) {
          const SearchNode* child = node.children[i];
          while(child->statsLock.test_and_set(std::memory_order_acquire));
          int64_t childVisits = child->stats.visits;
          child->statsLock.clear(std::memory_order_release);
          newNumVisits += childVisits;
        }
        //For the node's own visit itself
        newNumVisits += 1;

        //Set the visits in place
        while(node.statsLock.test_and_set(std::memory_order_acquire));
        node.stats.visits = newNumVisits;
        node.statsLock.clear(std::memory_order_release);

        //Update all other stats
        recomputeNodeStats(node, dummyThread, 0, 0, true);
      }
    }

    //Recursively update all stats in the tree if we have dynamic score values
    if(searchParams.dynamicScoreUtilityFactor != 0) {
      recursivelyRecomputeStats(node,dummyThread,true);
    }

  }
}

void Search::recursivelyRecomputeStats(SearchNode& node, SearchThread& thread, bool isRoot) {
  //First, recompute all children.
  vector<SearchNode*> children;
  children.reserve(rootBoard.x_size * rootBoard.y_size + 1);

  int numChildren;
  bool noNNOutput;
  {
    std::mutex& mutex = mutexPool->getMutex(node.lockIdx);
    lock_guard<std::mutex> lock(mutex);
    numChildren = node.numChildren;
    for(int i = 0; i<numChildren; i++)
      children.push_back(node.children[i]);

    noNNOutput = node.nnOutput == nullptr;
  }

  for(int i = 0; i<numChildren; i++) {
    recursivelyRecomputeStats(*(children[i]),thread,false);
  }

  //If this node has no nnOutput, then it must also have no children, because it's
  //a terminal node
  assert(!(noNNOutput && numChildren > 0));
  (void)noNNOutput; //avoid warning when we have no asserts

  //If the node has no children, then just update its utility directly
  if(numChildren <= 0) {
    while(node.statsLock.test_and_set(std::memory_order_acquire));
    double resultUtilitySum = node.stats.getResultUtilitySum(searchParams);
    double scoreMeanSum = node.stats.scoreMeanSum;
    double scoreMeanSqSum = node.stats.scoreMeanSqSum;
    double weightSum = node.stats.weightSum;
    int64_t numVisits = node.stats.visits;
    node.statsLock.clear(std::memory_order_release);

    //It's possible that this node has 0 weight in the case where it's the root node
    //and has 0 visits because we began a search and then stopped it before any playouts happened.
    //In that case, there's not much to recompute.
    if(weightSum <= 0.0) {
      assert(numVisits == 0);
      assert(isRoot);
    }
    else {
      double scoreUtility = getScoreUtility(scoreMeanSum, scoreMeanSqSum, weightSum);

      double newUtility = resultUtilitySum / weightSum + scoreUtility;
      double newUtilitySum = newUtility * weightSum;
      double newUtilitySqSum = newUtility * newUtility * weightSum;

      while(node.statsLock.test_and_set(std::memory_order_acquire));
      node.stats.utilitySum = newUtilitySum;
      node.stats.utilitySqSum = newUtilitySqSum;
      node.statsLock.clear(std::memory_order_release);
    }
  }
  else {
    //Otherwise recompute it using the usual method
    recomputeNodeStats(node, thread, 0, 0, isRoot);
  }
}


void Search::computeRootValues(Logger& logger) {
  //rootSafeArea is strictly pass-alive groups and strictly safe territory.
  bool nonPassAliveStones = false;
  bool safeBigTerritories = false;
  bool unsafeBigTerritories = false;
  bool isMultiStoneSuicideLegal = rootHistory.rules.multiStoneSuicideLegal;
  rootBoard.calculateArea(
    rootSafeArea,
    nonPassAliveStones,
    safeBigTerritories,
    unsafeBigTerritories,
    isMultiStoneSuicideLegal
  );

  //Figure out how to set recentScoreCenter
  {
    bool foundExpectedScoreFromTree = false;
    double expectedScore = 0.0;
    if(rootNode != NULL) {
      const SearchNode& node = *rootNode;
      while(node.statsLock.test_and_set(std::memory_order_acquire));
      double scoreMeanSum = node.stats.scoreMeanSum;
      double weightSum = node.stats.weightSum;
      int64_t numVisits = node.stats.visits;
      node.statsLock.clear(std::memory_order_release);
      if(numVisits > 0 && weightSum > 0) {
        foundExpectedScoreFromTree = true;
        expectedScore = scoreMeanSum / weightSum;
      }
    }

    //Grab a neural net evaluation for the current position and use that as the center
    if(!foundExpectedScoreFromTree) {
      Board board = rootBoard;
      const BoardHistory& hist = rootHistory;
      Player pla = rootPla;
      NNResultBuf nnResultBuf;
      bool skipCache = false;
      bool includeOwnerMap = true;
      bool isRoot = true;
      MiscNNInputParams nnInputParams;
      nnInputParams.drawEquivalentWinsForWhite = searchParams.drawEquivalentWinsForWhite;
      nnInputParams.conservativePass = isRoot && searchParams.conservativePass;
      if(searchParams.playoutDoublingAdvantage != 0) {
        Player playoutDoublingAdvantagePla = searchParams.playoutDoublingAdvantagePla == C_EMPTY ? rootPla : searchParams.playoutDoublingAdvantagePla;
        nnInputParams.playoutDoublingAdvantage = (
          getOpp(pla) == playoutDoublingAdvantagePla ? -searchParams.playoutDoublingAdvantage : searchParams.playoutDoublingAdvantage
        );
      }
      nnEvaluator->evaluate(
        board, hist, pla,
        nnInputParams,
        nnResultBuf, &logger, skipCache, includeOwnerMap
      );
      expectedScore = nnResultBuf.result->whiteScoreMean;
    }

    recentScoreCenter = expectedScore * (1.0 - searchParams.dynamicScoreCenterZeroWeight);
    double cap =  sqrt(rootBoard.x_size * rootBoard.y_size) * searchParams.dynamicScoreCenterScale;
    if(recentScoreCenter > expectedScore + cap)
      recentScoreCenter = expectedScore + cap;
    if(recentScoreCenter < expectedScore - cap)
      recentScoreCenter = expectedScore - cap;
  }
}

int64_t Search::numRootVisits() const {
  if(rootNode == NULL)
    return 0;
  while(rootNode->statsLock.test_and_set(std::memory_order_acquire));
  int64_t n = rootNode->stats.visits;
  rootNode->statsLock.clear(std::memory_order_release);
  return n;
}

void Search::addDirichletNoise(const SearchParams& searchParams, Rand& rand, int policySize, float* policyProbs) {
  int legalCount = 0;
  for(int i = 0; i<policySize; i++) {
    if(policyProbs[i] >= 0)
      legalCount += 1;
  }

  if(legalCount <= 0)
    throw StringError("addDirichletNoise: No move with nonnegative policy value - can't even pass?");

  //We're going to generate a gamma draw on each move with alphas that sum up to searchParams.rootDirichletNoiseTotalConcentration.
  //Half of the alpha weight are uniform.
  //The other half are shaped based on the log of the existing policy.
  double r[NNPos::MAX_NN_POLICY_SIZE];
  double logPolicySum = 0.0;
  for(int i = 0; i<policySize; i++) {
    if(policyProbs[i] >= 0) {
      r[i] = log(std::min(0.01, (double)policyProbs[i]) + 1e-20);
      logPolicySum += r[i];
    }
  }
  double logPolicyMean = logPolicySum / legalCount;
  double alphaPropSum = 0.0;
  for(int i = 0; i<policySize; i++) {
    if(policyProbs[i] >= 0) {
      r[i] = std::max(0.0, r[i] - logPolicyMean);
      alphaPropSum += r[i];
    }
  }
  double uniformProb = 1.0 / legalCount;
  if(alphaPropSum <= 0.0) {
    for(int i = 0; i<policySize; i++) {
      if(policyProbs[i] >= 0)
        r[i] = uniformProb;
    }
  }
  else {
    for(int i = 0; i<policySize; i++) {
      if(policyProbs[i] >= 0)
        r[i] = 0.5 * (r[i] / alphaPropSum + uniformProb);
    }
  }

  //r now contains the proportions with which we would like to split the alpha
  //The total of the alphas is searchParams.rootDirichletNoiseTotalConcentration
  //Generate gamma draw on each move
  double rSum = 0.0;
  for(int i = 0; i<policySize; i++) {
    if(policyProbs[i] >= 0) {
      r[i] = rand.nextGamma(r[i] * searchParams.rootDirichletNoiseTotalConcentration);
      rSum += r[i];
    }
    else
      r[i] = 0.0;
  }

  //Normalized gamma draws -> dirichlet noise
  for(int i = 0; i<policySize; i++)
    r[i] /= rSum;

  //At this point, r[i] contains a dirichlet distribution draw, so add it into the nnOutput.
  for(int i = 0; i<policySize; i++) {
    if(policyProbs[i] >= 0) {
      double weight = searchParams.rootDirichletNoiseWeight;
      policyProbs[i] = (float)(r[i] * weight + policyProbs[i] * (1.0-weight));
    }
  }
}


//Assumes node is locked
void Search::maybeAddPolicyNoiseAndTempAlreadyLocked(SearchThread& thread, SearchNode& node, bool isRoot) const {
  if(!isRoot)
    return;
  if(!searchParams.rootNoiseEnabled && searchParams.rootPolicyTemperature == 1.0 && searchParams.rootPolicyTemperatureEarly == 1.0)
    return;
  if(node.nnOutput->noisedPolicyProbs != NULL)
    return;

  //Copy nnOutput as we're about to modify its policy to add noise or temperature
  {
    shared_ptr<NNOutput> newNNOutput = std::make_shared<NNOutput>(*(node.nnOutput));
    //Replace the old pointer
    node.nnOutput = newNNOutput;
  }

  float* noisedPolicyProbs = new float[NNPos::MAX_NN_POLICY_SIZE];
  node.nnOutput->noisedPolicyProbs = noisedPolicyProbs;
  std::copy(node.nnOutput->policyProbs, node.nnOutput->policyProbs + NNPos::MAX_NN_POLICY_SIZE, noisedPolicyProbs);

  if(searchParams.rootPolicyTemperature != 1.0 || searchParams.rootPolicyTemperatureEarly != 1.0) {
    double rootPolicyTemperature = interpolateEarly(
      searchParams.chosenMoveTemperatureHalflife, searchParams.rootPolicyTemperatureEarly, searchParams.rootPolicyTemperature
    );

    double maxValue = 0.0;
    for(int i = 0; i<policySize; i++) {
      double prob = noisedPolicyProbs[i];
      if(prob > maxValue)
        maxValue = prob;
    }
    assert(maxValue > 0.0);

    double logMaxValue = log(maxValue);
    double invTemp = 1.0 / rootPolicyTemperature;
    double sum = 0.0;

    for(int i = 0; i<policySize; i++) {
      if(noisedPolicyProbs[i] > 0) {
        //Numerically stable way to raise to power and normalize
        float p = (float)exp((log((double)noisedPolicyProbs[i]) - logMaxValue) * invTemp);
        noisedPolicyProbs[i] = p;
        sum += p;
      }
    }
    assert(sum > 0.0);
    for(int i = 0; i<policySize; i++) {
      if(noisedPolicyProbs[i] >= 0) {
        noisedPolicyProbs[i] = (float)(noisedPolicyProbs[i] / sum);
      }
    }
  }

  if(searchParams.rootNoiseEnabled) {
    addDirichletNoise(searchParams, thread.rand, policySize, noisedPolicyProbs);
  }

}

bool Search::isAllowedRootMove(Loc moveLoc) const {
  assert(moveLoc == Board::PASS_LOC || rootBoard.isOnBoard(moveLoc));

  //For use on some online go servers, we want to be able to support a cleanup mode, where we force
  //the capture of stones that our training ruleset would consider simply dead by virtue of them
  //being pass-dead, so we add an option to forbid passing at the root.
  if(!rootPassLegal && moveLoc == Board::PASS_LOC)
    return false;
  //A bad situation that can happen that unnecessarily prolongs training games is where one player
  //repeatedly passes and the other side repeatedly fills the opponent's space and/or suicides over and over.
  //To mitigate some of this and save computation, we make it so that at the root, if the last four moves by the opponent
  //were passes, we will never play a move in either player's pass-alive area. In theory this could prune
  //a good move in situations like https://senseis.xmp.net/?1EyeFlaw, but this should be extraordinarly rare,
  if(searchParams.rootPruneUselessMoves &&
     rootHistory.moveHistory.size() > 0 &&
     moveLoc != Board::PASS_LOC
  ) {
    int lastIdx = rootHistory.moveHistory.size()-1;
    Player opp = getOpp(rootPla);
    if(lastIdx >= 6 &&
       rootHistory.moveHistory[lastIdx-0].loc == Board::PASS_LOC &&
       rootHistory.moveHistory[lastIdx-2].loc == Board::PASS_LOC &&
       rootHistory.moveHistory[lastIdx-4].loc == Board::PASS_LOC &&
       rootHistory.moveHistory[lastIdx-6].loc == Board::PASS_LOC &&
       rootHistory.moveHistory[lastIdx-0].pla == opp &&
       rootHistory.moveHistory[lastIdx-2].pla == opp &&
       rootHistory.moveHistory[lastIdx-4].pla == opp &&
       rootHistory.moveHistory[lastIdx-6].pla == opp &&
       (rootSafeArea[moveLoc] == opp || rootSafeArea[moveLoc] == rootPla))
      return false;
  }
  return true;
}

void Search::getValueChildWeights(
  int numChildren,
  //Unlike everywhere else where values are from white's perspective, values here are from one's own perspective
  const vector<double>& childSelfValuesBuf,
  const vector<int64_t>& childVisitsBuf,
  vector<double>& resultBuf
) const {
  resultBuf.clear();
  if(numChildren <= 0)
    return;
  if(numChildren == 1) {
    resultBuf.push_back(1.0);
    return;
  }

  assert(numChildren <= NNPos::MAX_NN_POLICY_SIZE);
  double stdevs[NNPos::MAX_NN_POLICY_SIZE];
  for(int i = 0; i<numChildren; i++) {
    int64_t numVisits = childVisitsBuf[i];
    assert(numVisits >= 0);
    if(numVisits == 0) {
      stdevs[i] = 0.0; //Unused
      continue;
    }

    double precision = 1.5 * sqrt((double)numVisits);

    //Ensure some minimum variance for stability regardless of how we change the above formula
    static const double minVariance = 0.00000001;
    stdevs[i] = sqrt(minVariance + 1.0 / precision);
  }

  double simpleValueSum = 0.0;
  int64_t numChildVisits = 0;
  for(int i = 0; i<numChildren; i++) {
    simpleValueSum += childSelfValuesBuf[i] * childVisitsBuf[i];
    numChildVisits += childVisitsBuf[i];
  }

  double simpleValue = simpleValueSum / numChildVisits;

  double weight[NNPos::MAX_NN_POLICY_SIZE];
  for(int i = 0; i<numChildren; i++) {
    if(childVisitsBuf[i] == 0) {
      weight[i] = 0.0;
      continue;
    }
    else {
      double z = (childSelfValuesBuf[i] - simpleValue) / stdevs[i];
      //Also just for numeric sanity, make sure everything has some tiny minimum value.
      weight[i] = valueWeightDistribution->getCdf(z) + 0.0001;
    }
  }

  //Post-process and normalize, to make sure we exactly have a probability distribution and sum exactly to 1.
  double totalWeight = 0.0;
  for(int i = 0; i<numChildren; i++) {
    double p = weight[i];
    totalWeight += p;
    resultBuf.push_back(p);
  }

  assert(totalWeight >= 0.0);
  if(totalWeight > 0) {
    for(int i = 0; i<numChildren; i++) {
      resultBuf[i] /= totalWeight;
    }
  }

}

static double cpuctExploration(int64_t totalChildVisits, const SearchParams& searchParams) {
  return searchParams.cpuctExploration +
    searchParams.cpuctExplorationLog * log((totalChildVisits + searchParams.cpuctExplorationBase) / searchParams.cpuctExplorationBase);
}

double Search::getExploreSelectionValue(
  double nnPolicyProb, int64_t totalChildVisits, int64_t childVisits,
  double childUtility, Player pla
) const {
  if(nnPolicyProb < 0)
    return POLICY_ILLEGAL_SELECTION_VALUE;

  double exploreComponent =
    cpuctExploration(totalChildVisits,searchParams)
    * nnPolicyProb
    * sqrt((double)totalChildVisits + 0.01) //TODO this is weird when totalChildVisits == 0, first exploration
    / (1.0 + childVisits);

  //At the last moment, adjust value to be from the player's perspective, so that players prefer values in their favor
  //rather than in white's favor
  double valueComponent = pla == P_WHITE ? childUtility : -childUtility;
  return exploreComponent + valueComponent;
}

//Return the childVisits that would make Search::getExploreSelectionValue return the given explore selection value.
//Or return 0, if it would be less than 0.
double Search::getExploreSelectionValueInverse(
  double exploreSelectionValue, double nnPolicyProb, int64_t totalChildVisits,
  double childUtility, Player pla
) const {
  if(nnPolicyProb < 0)
    return 0;
  double valueComponent = pla == P_WHITE ? childUtility : -childUtility;

  double exploreComponent = exploreSelectionValue - valueComponent;
  double exploreComponentScaling =
    cpuctExploration(totalChildVisits,searchParams)
    * nnPolicyProb
    * sqrt((double)totalChildVisits + 0.01); //TODO this is weird when totalChildVisits == 0, first exploration

  //Guard against float weirdness
  if(exploreComponent <= 0)
    return 1e100;

  double childVisits = exploreComponentScaling / exploreComponent - 1;
  if(childVisits < 0)
    childVisits = 0;
  return childVisits;
}


//Parent must be locked
double Search::getEndingWhiteScoreBonus(const SearchNode& parent, const SearchNode* child) const {
  if(&parent != rootNode || child->prevMoveLoc == Board::NULL_LOC)
    return 0.0;
  if(parent.nnOutput == nullptr || parent.nnOutput->whiteOwnerMap == NULL)
    return 0.0;

  bool isAreaIsh = rootHistory.rules.scoringRule == Rules::SCORING_AREA
    || (rootHistory.rules.scoringRule == Rules::SCORING_TERRITORY && rootHistory.encorePhase >= 2);
  assert(parent.nnOutput->nnXLen == nnXLen);
  assert(parent.nnOutput->nnYLen == nnYLen);
  float* whiteOwnerMap = parent.nnOutput->whiteOwnerMap;
  Loc moveLoc = child->prevMoveLoc;

  const double extreme = 0.95;
  const double tail = 0.05;

  //Extra points from the perspective of the root player
  double extraRootPoints = 0.0;
  if(isAreaIsh) {
    //Areaish scoring - in an effort to keep the game short and slightly discourage pointless territory filling at the end
    //discourage any move that, except in case of ko, is either:
    // * On a spot that the opponent almost surely owns
    // * On a spot that the player almost surely owns and it is not adjacent to opponent stones and is not a connection of non-pass-alive groups.
    //These conditions should still make it so that "cleanup" and dame-filling moves are not discouraged.
    if(moveLoc != Board::PASS_LOC && rootBoard.ko_loc == Board::NULL_LOC) {
      int pos = NNPos::locToPos(moveLoc,rootBoard.x_size,nnXLen,nnYLen);
      double plaOwnership = rootPla == P_WHITE ? whiteOwnerMap[pos] : -whiteOwnerMap[pos];
      //if(rootSafeArea[moveLoc] == rootPla) plaOwnership = 1.0;
      //if(rootSafeArea[moveLoc] == getOpp(rootPla)) plaOwnership = -1.0;

      if(plaOwnership <= -extreme)
        extraRootPoints -= searchParams.rootEndingBonusPoints * ((-extreme - plaOwnership) / tail);
      else if(plaOwnership >= extreme) {
        if(!rootBoard.isAdjacentToPla(moveLoc,getOpp(rootPla)) &&
           !rootBoard.isNonPassAliveSelfConnection(moveLoc,rootPla,rootSafeArea)) {
          extraRootPoints -= searchParams.rootEndingBonusPoints * ((plaOwnership - extreme) / tail);
        }
      }
    }
  }
  else {
    //Territorish scoring - slightly encourage dame-filling by discouraging passing, so that the player will try to do everything
    //non-point-losing first, like filling dame.
    //Human japanese rules often "want" you to fill the dame so this is a cosmetic adjustment to encourage the neural
    //net to learn to do so in the main phase rather than waiting until the encore.
    //But cosmetically, it's also not great if we just encourage useless threat moves in the opponent's territory to prolong the game.
    //So also discourage those moves except in cases of ko. Also similar to area scoring just to be symmetrical, discourage moves on spots
    //that the player almost surely owns that are not adjacent to opponent stones and are not a connection of non-pass-alive groups.
    if(moveLoc == Board::PASS_LOC)
      extraRootPoints -= searchParams.rootEndingBonusPoints * (2.0/3.0);
    else if(rootBoard.ko_loc == Board::NULL_LOC) {
      int pos = NNPos::locToPos(moveLoc,rootBoard.x_size,nnXLen,nnYLen);
      double plaOwnership = rootPla == P_WHITE ? whiteOwnerMap[pos] : -whiteOwnerMap[pos];
      if(plaOwnership <= -extreme)
        extraRootPoints -= searchParams.rootEndingBonusPoints * ((-extreme - plaOwnership) / tail);
      else if(plaOwnership >= extreme) {
        if(!rootBoard.isAdjacentToPla(moveLoc,getOpp(rootPla)) &&
           !rootBoard.isNonPassAliveSelfConnection(moveLoc,rootPla,rootSafeArea)) {
          extraRootPoints -= searchParams.rootEndingBonusPoints * ((plaOwnership - extreme) / tail);
        }
      }
    }
  }

  if(rootPla == P_WHITE)
    return extraRootPoints;
  else
    return -extraRootPoints;
}

static bool nearWeakStones(const Board& board, Loc loc, Player pla) {
  Player opp = getOpp(pla);
  for(int i = 0; i < 4; i++) {
    Loc adj = loc + board.adj_offsets[i];
    if(board.colors[adj] == opp && board.getNumLiberties(adj) <= 4)
      return true;
    else if(board.colors[adj] == C_EMPTY) {
      for(int j = 0; j < 4; j++) {
        Loc adjadj = adj + board.adj_offsets[j];
        if(board.colors[adjadj] == opp) {
          if(board.getNumLiberties(adjadj) <= 3)
            return true;
        }
      }
    }
  }
  return false;
}

float Search::adjustExplorePolicyProb(
  const SearchThread& thread, const SearchNode& parent, Loc moveLoc, float nnPolicyProb,
  double parentUtility, double totalChildVisits, double childVisits, double& childUtility
) const {
  (void)totalChildVisits;
  //Near the tree root, explore local moves a bit more for root player
  if(searchParams.localExplore &&
     parent.nextPla == rootPla && thread.history.moveHistory.size() > 0 &&
     thread.history.moveHistory.size() <= rootHistory.moveHistory.size() + 2
  ) {
    Loc prevLoc = thread.history.moveHistory[thread.history.moveHistory.size()-1].loc;
    if(moveLoc != Board::PASS_LOC && prevLoc != Board::PASS_LOC) {
      //Within sqrt(5) of the opponent's move
      //Within 2 of a group with <= 3 liberties, or touching opp stone with <= 4 liberties
      //Not self atari
      const Board& board = thread.board;
      int distanceSq = Location::euclideanDistanceSquared(moveLoc,prevLoc,board.x_size);
      if(distanceSq <= 5 && board.getNumLibertiesAfterPlay(moveLoc,parent.nextPla,2) >= 2) {
        if(nearWeakStones(board, moveLoc, parent.nextPla)) {
          float averageToward = distanceSq <= 2 ? 0.06f : distanceSq <= 4 ? 0.04f : 0.03f;
          //Behave as if policy is a few points higher
          if(nnPolicyProb < averageToward)
            nnPolicyProb = 0.5f * (nnPolicyProb + averageToward);
          if(childVisits > 0 && (parent.nextPla == P_WHITE ? (childUtility < parentUtility) : (childUtility > parentUtility))) {
            //Also encourage a bit more exploration even if value is bad
            double parentWeight = sqrt(childVisits);
            childUtility = (parentUtility * parentWeight + childUtility * childVisits) / (parentWeight + childVisits);
          }
        }
      }
    }
  }
  return nnPolicyProb;
}

int Search::getPos(Loc moveLoc) const {
  return NNPos::locToPos(moveLoc,rootBoard.x_size,nnXLen,nnYLen);
}

//Parent must be locked
double Search::getExploreSelectionValue(
  const SearchNode& parent, const float* parentPolicyProbs, const SearchNode* child,
  int64_t totalChildVisits, double fpuValue, double parentUtility,
  bool isDuringSearch, const SearchThread* thread
) const {
  Loc moveLoc = child->prevMoveLoc;
  int movePos = getPos(moveLoc);
  float nnPolicyProb = parentPolicyProbs[movePos];

  while(child->statsLock.test_and_set(std::memory_order_acquire));
  int64_t childVisits = child->stats.visits;
  double utilitySum = child->stats.utilitySum;
  double scoreMeanSum = child->stats.scoreMeanSum;
  double scoreMeanSqSum = child->stats.scoreMeanSqSum;
  double weightSum = child->stats.weightSum;
  int32_t childVirtualLosses = child->virtualLosses;
  child->statsLock.clear(std::memory_order_release);

  //It's possible that childVisits is actually 0 here with multithreading because we're visiting this node while a child has
  //been expanded but its thread not yet finished its first visit
  double childUtility;
  if(childVisits <= 0)
    childUtility = fpuValue;
  else {
    assert(weightSum > 0.0);
    childUtility = utilitySum / weightSum;

    //Tiny adjustment for passing
    double endingScoreBonus = getEndingWhiteScoreBonus(parent,child);
    if(endingScoreBonus != 0)
      childUtility += getScoreUtilityDiff(scoreMeanSum, scoreMeanSqSum, weightSum, endingScoreBonus);
  }

  //When multithreading, totalChildVisits could be out of sync with childVisits, so if they provably are, then fix that up
  if(totalChildVisits < childVisits)
    totalChildVisits = childVisits;

  //Virtual losses to direct threads down different paths
  if(childVirtualLosses > 0) {
    //totalChildVisits += childVirtualLosses; //Should get better thread dispersal without this
    childVisits += childVirtualLosses;
    double utilityRadius = searchParams.winLossUtilityFactor + searchParams.staticScoreUtilityFactor + searchParams.dynamicScoreUtilityFactor;
    double virtualLossUtility = (parent.nextPla == P_WHITE ? -utilityRadius : utilityRadius);
    double virtualLossVisitFrac = (double)childVirtualLosses / childVisits;
    childUtility = childUtility + (virtualLossUtility - childUtility) * virtualLossVisitFrac;
  }

  if(isDuringSearch)
    nnPolicyProb = adjustExplorePolicyProb(*thread,parent,moveLoc,nnPolicyProb,parentUtility,totalChildVisits,childVisits,childUtility);

  //Hack to get the root to funnel more visits down child branches
  if(isDuringSearch && (&parent == rootNode) && searchParams.rootDesiredPerChildVisitsCoeff > 0.0) {
    if(childVisits < sqrt(nnPolicyProb * totalChildVisits * searchParams.rootDesiredPerChildVisitsCoeff)) {
      return 1e20;
    }
  }

  return getExploreSelectionValue(nnPolicyProb,totalChildVisits,childVisits,childUtility,parent.nextPla);
}

double Search::getNewExploreSelectionValue(const SearchNode& parent, float nnPolicyProb, int64_t totalChildVisits, double fpuValue) const {
  int64_t childVisits = 0;
  double childUtility = fpuValue;
  return getExploreSelectionValue(nnPolicyProb,totalChildVisits,childVisits,childUtility,parent.nextPla);
}

//Parent must be locked
int64_t Search::getReducedPlaySelectionVisits(
  const SearchNode& parent, const float* parentPolicyProbs, const SearchNode* child,
  int64_t totalChildVisits, double bestChildExploreSelectionValue
) const {
  assert(&parent == rootNode);
  Loc moveLoc = child->prevMoveLoc;
  int movePos = getPos(moveLoc);
  float nnPolicyProb = parentPolicyProbs[movePos];

  while(child->statsLock.test_and_set(std::memory_order_acquire));
  int64_t childVisits = child->stats.visits;
  double utilitySum = child->stats.utilitySum;
  double scoreMeanSum = child->stats.scoreMeanSum;
  double scoreMeanSqSum = child->stats.scoreMeanSqSum;
  double weightSum = child->stats.weightSum;
  child->statsLock.clear(std::memory_order_release);

  //Child visits may be 0 if this function is called in a multithreaded context, such as during live analysis
  if(childVisits <= 0)
    return 0;
  assert(weightSum > 0.0);

  //Tiny adjustment for passing
  double endingScoreBonus = getEndingWhiteScoreBonus(parent,child);
  double childUtility = utilitySum / weightSum;
  if(endingScoreBonus != 0)
    childUtility += getScoreUtilityDiff(scoreMeanSum, scoreMeanSqSum, weightSum, endingScoreBonus);

  double childVisitsWeRetrospectivelyWanted = getExploreSelectionValueInverse(
    bestChildExploreSelectionValue, nnPolicyProb, totalChildVisits, childUtility, parent.nextPla
  );
  if(childVisits > childVisitsWeRetrospectivelyWanted)
    childVisits = (int64_t)ceil(childVisitsWeRetrospectivelyWanted);
  return childVisits;
}

double Search::getFpuValueForChildrenAssumeVisited(const SearchNode& node, Player pla, bool isRoot, double policyProbMassVisited, double& parentUtility) const {
  if(searchParams.fpuUseParentAverage) {
    while(node.statsLock.test_and_set(std::memory_order_acquire));
    double utilitySum = node.stats.utilitySum;
    double weightSum = node.stats.weightSum;
    node.statsLock.clear(std::memory_order_release);

    assert(weightSum > 0.0);
    parentUtility = utilitySum / weightSum;
  }
  else {
    parentUtility = getUtilityFromNN(*node.nnOutput);
  }

  double fpuValue;
  {
    double fpuReductionMax = isRoot ? searchParams.rootFpuReductionMax : searchParams.fpuReductionMax;
    double fpuLossProp = isRoot ? searchParams.rootFpuLossProp : searchParams.fpuLossProp;
    double utilityRadius = searchParams.winLossUtilityFactor + searchParams.staticScoreUtilityFactor + searchParams.dynamicScoreUtilityFactor;

    double reduction = fpuReductionMax * sqrt(policyProbMassVisited);
    fpuValue = pla == P_WHITE ? parentUtility - reduction : parentUtility + reduction;
    double lossValue = pla == P_WHITE ? -utilityRadius : utilityRadius;
    fpuValue = fpuValue + (lossValue - fpuValue) * fpuLossProp;
  }

  return fpuValue;
}


//Assumes node is locked
void Search::selectBestChildToDescend(
  const SearchThread& thread, const SearchNode& node, int& bestChildIdx, Loc& bestChildMoveLoc,
  bool posesWithChildBuf[NNPos::MAX_NN_POLICY_SIZE],
  bool isRoot) const
{
  assert(thread.pla == node.nextPla);

  double maxSelectionValue = POLICY_ILLEGAL_SELECTION_VALUE;
  bestChildIdx = -1;
  bestChildMoveLoc = Board::NULL_LOC;

  int numChildren = node.numChildren;

  double policyProbMassVisited = 0.0;
  int64_t totalChildVisits = 0;
  float* policyProbs = node.nnOutput->getPolicyProbsMaybeNoised();
  for(int i = 0; i<numChildren; i++) {
    const SearchNode* child = node.children[i];
    Loc moveLoc = child->prevMoveLoc;
    int movePos = getPos(moveLoc);
    float nnPolicyProb = policyProbs[movePos];
    policyProbMassVisited += nnPolicyProb;

    while(child->statsLock.test_and_set(std::memory_order_acquire));
    int64_t childVisits = child->stats.visits;
    child->statsLock.clear(std::memory_order_release);

    totalChildVisits += childVisits;
  }
  //Probability mass should not sum to more than 1, giving a generous allowance
  //for floating point error.
  assert(policyProbMassVisited <= 1.0001);

  //First play urgency
  double parentUtility;
  double fpuValue = getFpuValueForChildrenAssumeVisited(node, thread.pla, isRoot, policyProbMassVisited, parentUtility);

  std::fill(posesWithChildBuf,posesWithChildBuf+NNPos::MAX_NN_POLICY_SIZE,false);

  //Try all existing children
  for(int i = 0; i<numChildren; i++) {
    const SearchNode* child = node.children[i];
    Loc moveLoc = child->prevMoveLoc;
    bool isDuringSearch = true;
    double selectionValue = getExploreSelectionValue(node,policyProbs,child,totalChildVisits,fpuValue,parentUtility,isDuringSearch,&thread);
    if(selectionValue > maxSelectionValue) {
      maxSelectionValue = selectionValue;
      bestChildIdx = i;
      bestChildMoveLoc = moveLoc;
    }

    posesWithChildBuf[getPos(moveLoc)] = true;
  }

  //Try the new child with the best policy value
  Loc bestNewMoveLoc = Board::NULL_LOC;
  float bestNewNNPolicyProb = -1.0f;
  for(int movePos = 0; movePos<policySize; movePos++) {
    bool alreadyTried = posesWithChildBuf[movePos];
    if(alreadyTried)
      continue;

    Loc moveLoc = NNPos::posToLoc(movePos,thread.board.x_size,thread.board.y_size,nnXLen,nnYLen);
    if(moveLoc == Board::NULL_LOC)
      continue;

    //Special logic for the root
    if(isRoot) {
      assert(thread.board.pos_hash == rootBoard.pos_hash);
      assert(thread.pla == rootPla);
      if(!isAllowedRootMove(moveLoc))
        continue;
    }

    float nnPolicyProb = policyProbs[movePos];
    double childUtility = 0.0; //dummy, we don't care
    nnPolicyProb = adjustExplorePolicyProb(thread,node,moveLoc,nnPolicyProb,parentUtility,totalChildVisits,0,childUtility);

    if(nnPolicyProb > bestNewNNPolicyProb) {
      bestNewNNPolicyProb = nnPolicyProb;
      bestNewMoveLoc = moveLoc;
    }
  }
  if(bestNewMoveLoc != Board::NULL_LOC) {
    double selectionValue = getNewExploreSelectionValue(node,bestNewNNPolicyProb,totalChildVisits,fpuValue);
    if(selectionValue > maxSelectionValue) {
      maxSelectionValue = selectionValue;
      bestChildIdx = numChildren;
      bestChildMoveLoc = bestNewMoveLoc;
    }
  }

}
void Search::updateStatsAfterPlayout(SearchNode& node, SearchThread& thread, int32_t virtualLossesToSubtract, bool isRoot) {
  recomputeNodeStats(node,thread,1,virtualLossesToSubtract,isRoot);
}

//Recompute all the stats of this node based on its children, except its visits and virtual losses, which are not child-dependent and
//are updated in the manner specified.
//Assumes this node has an nnOutput
void Search::recomputeNodeStats(SearchNode& node, SearchThread& thread, int numVisitsToAdd, int32_t virtualLossesToSubtract, bool isRoot) {
  //Find all children and compute weighting of the children based on their values
  vector<double>& weightFactors = thread.weightFactorBuf;
  vector<double>& winValues = thread.winValuesBuf;
  vector<double>& noResultValues = thread.noResultValuesBuf;
  vector<double>& scoreMeans = thread.scoreMeansBuf;
  vector<double>& scoreMeanSqs = thread.scoreMeanSqsBuf;
  vector<double>& leads = thread.leadsBuf;
  vector<double>& utilitySums = thread.utilityBuf;
  vector<double>& utilitySqSums = thread.utilitySqBuf;
  vector<double>& selfUtilities = thread.selfUtilityBuf;
  vector<double>& weightSums = thread.weightBuf;
  vector<double>& weightSqSums = thread.weightSqBuf;
  vector<int64_t>& visits = thread.visitsBuf;

  int64_t totalChildVisits = 0;
  int64_t maxChildVisits = 0;

  std::mutex& mutex = mutexPool->getMutex(node.lockIdx);
  unique_lock<std::mutex> lock(mutex);

  int numChildren = node.numChildren;
  int numGoodChildren = 0;
  for(int i = 0; i<numChildren; i++) {
    const SearchNode* child = node.children[i];

    while(child->statsLock.test_and_set(std::memory_order_acquire));
    int64_t childVisits = child->stats.visits;
    double winValueSum = child->stats.winValueSum;
    double noResultValueSum = child->stats.noResultValueSum;
    double scoreMeanSum = child->stats.scoreMeanSum;
    double scoreMeanSqSum = child->stats.scoreMeanSqSum;
    double leadSum = child->stats.leadSum;
    double weightSum = child->stats.weightSum;
    double weightSqSum = child->stats.weightSqSum;
    double utilitySum = child->stats.utilitySum;
    double utilitySqSum = child->stats.utilitySqSum;
    child->statsLock.clear(std::memory_order_release);

    if(childVisits <= 0)
      continue;
    assert(weightSum > 0.0);

    double childUtility = utilitySum / weightSum;

    winValues[numGoodChildren] = winValueSum / weightSum;
    noResultValues[numGoodChildren] = noResultValueSum / weightSum;
    scoreMeans[numGoodChildren] = scoreMeanSum / weightSum;
    scoreMeanSqs[numGoodChildren] = scoreMeanSqSum / weightSum;
    leads[numGoodChildren] = leadSum / weightSum;
    utilitySums[numGoodChildren] = utilitySum;
    utilitySqSums[numGoodChildren] = utilitySqSum;
    selfUtilities[numGoodChildren] = node.nextPla == P_WHITE ? childUtility : -childUtility;
    weightSums[numGoodChildren] = weightSum;
    weightSqSums[numGoodChildren] = weightSqSum;
    visits[numGoodChildren] = childVisits;
    totalChildVisits += childVisits;

    if(childVisits > maxChildVisits)
      maxChildVisits = childVisits;
    numGoodChildren++;
  }
  lock.unlock();

  if(searchParams.valueWeightExponent > 0)
    getValueChildWeights(numGoodChildren,selfUtilities,visits,weightFactors);

  //In the case we're enabling noise at the root node, also apply the slight subtraction
  //of visits from the root node's children so as to downweight the effect of the few dozen visits
  //we send towards children that are so bad that we never try them even once again.

  //One slightly surprising behavior is that this slight subtraction won't happen in the case where
  //we have just promoted a child to the root due to preservation of the tree across moves
  //but we haven't sent any playouts through the root yet. But having rootNoiseEnabled without
  //clearing the tree every search is a bit weird anyways.
  double amountToSubtract = 0.0;
  double amountToPrune = 0.0;
  if(isRoot && searchParams.rootNoiseEnabled) {
    amountToSubtract = std::min(searchParams.chosenMoveSubtract, maxChildVisits/64.0);
    amountToPrune = std::min(searchParams.chosenMovePrune, maxChildVisits/64.0);
  }

  double winValueSum = 0.0;
  double noResultValueSum = 0.0;
  double scoreMeanSum = 0.0;
  double scoreMeanSqSum = 0.0;
  double leadSum = 0.0;
  double utilitySum = 0.0;
  double utilitySqSum = 0.0;
  double weightSum = 0.0;
  double weightSqSum = 0.0;
  for(int i = 0; i<numGoodChildren; i++) {
    if(visits[i] < amountToPrune)
      continue;
    double desiredWeight = (double)visits[i] - amountToSubtract;
    if(desiredWeight < 0.0)
      continue;

    if(searchParams.valueWeightExponent > 0)
      desiredWeight *= pow(weightFactors[i], searchParams.valueWeightExponent);

    double weightScaling = desiredWeight / weightSums[i];

    winValueSum += desiredWeight * winValues[i];
    noResultValueSum += desiredWeight * noResultValues[i];
    scoreMeanSum += desiredWeight * scoreMeans[i];
    scoreMeanSqSum += desiredWeight * scoreMeanSqs[i];
    leadSum += desiredWeight * leads[i];
    utilitySum += weightScaling * utilitySums[i];
    utilitySqSum += weightScaling * utilitySqSums[i];
    weightSum += desiredWeight;
    weightSqSum += weightScaling * weightScaling * weightSqSums[i];
  }

  //Also add in the direct evaluation of this node.
  {
    //Since we've scaled all the child weights in some arbitrary way, adjust and make sure
    //that the direct evaluation of the node still has precisely 1/N weight.
    //Do some things to carefully avoid divide by 0.
    double desiredWeight = (totalChildVisits > 0) ? weightSum / totalChildVisits : weightSum;
    if(desiredWeight < 0.0001) //Just in case
      desiredWeight = 0.0001;

    double winProb = (double)node.nnOutput->whiteWinProb;
    double noResultProb = (double)node.nnOutput->whiteNoResultProb;
    double scoreMean = (double)node.nnOutput->whiteScoreMean;
    double scoreMeanSq = (double)node.nnOutput->whiteScoreMeanSq;
    double lead = (double)node.nnOutput->whiteLead;
    double utility =
      getResultUtility(winProb, noResultProb)
      + getScoreUtility(scoreMean, scoreMeanSq, 1.0);

    winValueSum += winProb * desiredWeight;
    noResultValueSum += noResultProb * desiredWeight;
    scoreMeanSum += scoreMean * desiredWeight;
    scoreMeanSqSum += scoreMeanSq * desiredWeight;
    leadSum += lead * desiredWeight;
    utilitySum += utility * desiredWeight;
    utilitySqSum += utility * utility * desiredWeight;
    weightSum += desiredWeight;
    weightSqSum += desiredWeight * desiredWeight;
  }

  while(node.statsLock.test_and_set(std::memory_order_acquire));
  node.stats.visits += numVisitsToAdd;
  //It's possible that these values are a bit wrong if there's a race and two threads each try to update this
  //each of them only having some of the latest updates for all the children. We just accept this and let the
  //error persist, it will get fixed the next time a visit comes through here and the values will at least
  //be consistent with each other within this node, since statsLock at least ensures these three are set atomically.
  node.stats.winValueSum = winValueSum;
  node.stats.noResultValueSum = noResultValueSum;
  node.stats.scoreMeanSum = scoreMeanSum;
  node.stats.scoreMeanSqSum = scoreMeanSqSum;
  node.stats.leadSum = leadSum;
  node.stats.utilitySum = utilitySum;
  node.stats.utilitySqSum = utilitySqSum;
  node.stats.weightSum = weightSum;
  node.stats.weightSqSum = weightSqSum;
  node.virtualLosses -= virtualLossesToSubtract;
  node.statsLock.clear(std::memory_order_release);
}

void Search::runSinglePlayout(SearchThread& thread) {
  bool posesWithChildBuf[NNPos::MAX_NN_POLICY_SIZE];
  playoutDescend(thread,*rootNode,posesWithChildBuf,true,0);

  //Restore thread state back to the root state
  thread.pla = rootPla;
  thread.board = rootBoard;
  thread.history = rootHistory;
}

void Search::addLeafValue(SearchNode& node, double winValue, double noResultValue, double scoreMean, double scoreMeanSq, double lead, int32_t virtualLossesToSubtract) {
  double utility =
    getResultUtility(winValue, noResultValue)
    + getScoreUtility(scoreMean, scoreMeanSq, 1.0);

  while(node.statsLock.test_and_set(std::memory_order_acquire));
  node.stats.visits += 1;
  node.stats.winValueSum += winValue;
  node.stats.noResultValueSum += noResultValue;
  node.stats.scoreMeanSum += scoreMean;
  node.stats.scoreMeanSqSum += scoreMeanSq;
  node.stats.leadSum += lead;
  node.stats.utilitySum += utility;
  node.stats.utilitySqSum += utility * utility;
  node.stats.weightSum += 1.0;
  node.stats.weightSqSum += 1.0;
  node.virtualLosses -= virtualLossesToSubtract;
  node.statsLock.clear(std::memory_order_release);
}

//Assumes node is locked
//Assumes node already has an nnOutput
void Search::maybeRecomputeExistingNNOutput(
  SearchThread& thread, SearchNode& node, bool isRoot
) {
  //Right now only the root node currently ever needs to recompute, and only if it's old
  if(isRoot && node.nnOutputAge != searchNodeAge) {
    node.nnOutputAge = searchNodeAge;

    //Recompute if we have no ownership map, since we need it for getEndingWhiteScoreBonus
    //If conservative passing, then we may also need to recompute the root policy ignoring the history if a pass ends the game
    //If averaging a bunch of symmetries, then we need to recompute it too
    if(node.nnOutput->whiteOwnerMap == NULL ||
       (searchParams.conservativePass && thread.history.passWouldEndGame(thread.board,thread.pla)) ||
       searchParams.rootNumSymmetriesToSample > 1
    ) {
      initNodeNNOutput(thread,node,isRoot,false,0,true);
      assert(node.nnOutput->whiteOwnerMap != NULL);
    }
    //We also need to recompute the root nn if we have root noise or temperature and that's missing.
    else {
      //We don't need to go all the way to the nnEvaluator, we just need to maybe add those transforms
      //to the existing policy.
      maybeAddPolicyNoiseAndTempAlreadyLocked(thread,node,isRoot);
    }
  }
}

//Assumes node is locked
void Search::initNodeNNOutput(
  SearchThread& thread, SearchNode& node,
  bool isRoot, bool skipCache, int32_t virtualLossesToSubtract, bool isReInit
) {
  bool includeOwnerMap = isRoot || alwaysIncludeOwnerMap;
  MiscNNInputParams nnInputParams;
  nnInputParams.drawEquivalentWinsForWhite = searchParams.drawEquivalentWinsForWhite;
  nnInputParams.conservativePass = isRoot && searchParams.conservativePass;
  if(searchParams.playoutDoublingAdvantage != 0) {
    Player playoutDoublingAdvantagePla = searchParams.playoutDoublingAdvantagePla == C_EMPTY ? rootPla : searchParams.playoutDoublingAdvantagePla;
    nnInputParams.playoutDoublingAdvantage = (
      getOpp(thread.pla) == playoutDoublingAdvantagePla ? -searchParams.playoutDoublingAdvantage : searchParams.playoutDoublingAdvantage
    );
  }
  if(isRoot && searchParams.rootNumSymmetriesToSample > 1) {
    vector<shared_ptr<NNOutput>> ptrs;
    for(int i = 0; i<searchParams.rootNumSymmetriesToSample; i++) {
      bool skipCacheThisIteration = skipCache || i > 0; //Skip cache on subsequent iterations to get new random draws for orientation
      nnEvaluator->evaluate(
        thread.board, thread.history, thread.pla,
        nnInputParams,
        thread.nnResultBuf, thread.logger, skipCacheThisIteration, includeOwnerMap
      );
      ptrs.push_back(std::move(thread.nnResultBuf.result));
    }
    node.nnOutput = std::shared_ptr<NNOutput>(new NNOutput(ptrs));
  }
  else {
    nnEvaluator->evaluate(
      thread.board, thread.history, thread.pla,
      nnInputParams,
      thread.nnResultBuf, thread.logger, skipCache, includeOwnerMap
    );
    node.nnOutput = std::move(thread.nnResultBuf.result);
  }

  maybeAddPolicyNoiseAndTempAlreadyLocked(thread,node,isRoot);
  node.nnOutputAge = searchNodeAge;

  //If this is a re-initialization of the nnOutput, we don't want to add any visits or anything.
  //Also don't bother updating any of the stats. Technically we should do so because winValueSum
  //and such will have changed potentially due to a new orientation of the neural net eval
  //slightly affecting the evals, but this is annoying to recompute from scratch, and on the next
  //visit updateStatsAfterPlayout should fix it all up anyways.
  if(isReInit)
    return;

  //Values in the search are from the perspective of white positive always
  double winProb = (double)node.nnOutput->whiteWinProb;
  double noResultProb = (double)node.nnOutput->whiteNoResultProb;
  double scoreMean = (double)node.nnOutput->whiteScoreMean;
  double scoreMeanSq = (double)node.nnOutput->whiteScoreMeanSq;
  double lead = (double)node.nnOutput->whiteLead;

  addLeafValue(node,winProb,noResultProb,scoreMean,scoreMeanSq,lead,virtualLossesToSubtract);
}

void Search::playoutDescend(
  SearchThread& thread, SearchNode& node,
  bool posesWithChildBuf[NNPos::MAX_NN_POLICY_SIZE],
  bool isRoot, int32_t virtualLossesToSubtract
) {
  //Hit terminal node, finish
  //In the case where we're forcing the search to make another move at the root, don't terminate, actually run search for a move more.
  //In the case where we're conservativePass and the game just ended due to a root pass, actually let it keep going.
  //Note that in the second case with tree reuse we can end up with a weird situation where a terminal node becomes nonterminal due
  //to now being a child of the root! This is okay - subsequent visits to the node will fall through to initNodeNNOutput, and we will
  //have a weird leaf node with 2 visits worth of mixed terminal and nn values, but further visits will even hit recomputeNodeStats
  //which should clean it all it.
  if(!isRoot && thread.history.isGameFinished /*&& gomoku
     !(searchParams.conservativePass &&
       thread.history.moveHistory.size() == rootHistory.moveHistory.size() + 1 &&
       node.prevMoveLoc == Board::PASS_LOC)*/
  ) {
    if(thread.history.isNoResult) {
      double winValue = 0.0;
      double noResultValue = 1.0;
      double scoreMean = 0.0;
      double scoreMeanSq = 0.0;
      double lead = 0.0;
      addLeafValue(node, winValue, noResultValue, scoreMean, scoreMeanSq, lead, virtualLossesToSubtract);
      return;
    }
    else {
      double winValue = ScoreValue::whiteWinsOfWinner(thread.history.winner, searchParams.drawEquivalentWinsForWhite);
      double noResultValue = 0.0;
      double scoreMean = ScoreValue::whiteScoreDrawAdjust(thread.history.finalWhiteMinusBlackScore,searchParams.drawEquivalentWinsForWhite,thread.history);
      double scoreMeanSq = ScoreValue::whiteScoreMeanSqOfScoreGridded(thread.history.finalWhiteMinusBlackScore,searchParams.drawEquivalentWinsForWhite);
      double lead = scoreMean;
      addLeafValue(node, winValue, noResultValue, scoreMean, scoreMeanSq, lead, virtualLossesToSubtract);
      return;
    }
  }

  std::mutex& mutex = mutexPool->getMutex(node.lockIdx);
  unique_lock<std::mutex> lock(mutex);

  //Hit leaf node, finish
  if(node.nnOutput == nullptr) {
    initNodeNNOutput(thread,node,isRoot,false,virtualLossesToSubtract,false);
    return;
  }

  maybeRecomputeExistingNNOutput(thread,node,isRoot);

  //Not leaf node, so recurse

  //Find the best child to descend down
  int bestChildIdx;
  Loc bestChildMoveLoc;
  selectBestChildToDescend(thread,node,bestChildIdx,bestChildMoveLoc,posesWithChildBuf,isRoot);

  //The absurdly rare case that the move chosen is not legal
  //(this should only happen either on a bug or where the nnHash doesn't have full legality information or when there's an actual hash collision).
  //Regenerate the neural net call and continue
  if(!thread.history.isLegal(thread.board,bestChildMoveLoc,thread.pla)) {
    bool isReInit = true;
    initNodeNNOutput(thread,node,isRoot,true,0,isReInit);

    if(thread.logStream != NULL)
      (*thread.logStream) << "WARNING: Chosen move not legal so regenerated nn output, nnhash=" << node.nnOutput->nnHash << endl;

    //As isReInit is true, we don't return, just keep going, since we didn't count this as a true visit in the node stats
    selectBestChildToDescend(thread,node,bestChildIdx,bestChildMoveLoc,posesWithChildBuf,isRoot);
    //We should absolutely be legal this time
    assert(thread.history.isLegal(thread.board,bestChildMoveLoc,thread.pla));
  }

  if(bestChildIdx < -1) {
    lock.unlock();
    throw StringError("Search error: No move with sane selection value - can't even pass?");
  }

  //Reallocate the children array to increase capacity if necessary
  if(bestChildIdx >= node.childrenCapacity) {
    int newCapacity = node.childrenCapacity + (node.childrenCapacity / 4) + 1;
    assert(newCapacity < 0x3FFF);
    SearchNode** newArr = new SearchNode*[newCapacity];
    for(int i = 0; i<node.numChildren; i++) {
      newArr[i] = node.children[i];
      node.children[i] = NULL;
    }
    SearchNode** oldArr = node.children;
    node.children = newArr;
    node.childrenCapacity = (uint16_t)newCapacity;
    delete[] oldArr;
  }

  Loc moveLoc = bestChildMoveLoc;

  //Allocate a new child node if necessary
  SearchNode* child;
  if(bestChildIdx == node.numChildren) {
    assert(thread.history.isLegal(thread.board,moveLoc,thread.pla));
    thread.history.makeBoardMoveAssumeLegal(thread.board,moveLoc,thread.pla,rootKoHashTable);
    thread.pla = getOpp(thread.pla);

    node.numChildren++;
    child = new SearchNode(*this,thread,moveLoc);
    node.children[bestChildIdx] = child;

    while(child->statsLock.test_and_set(std::memory_order_acquire));
    child->virtualLosses += searchParams.numVirtualLossesPerThread;
    child->statsLock.clear(std::memory_order_release);

    lock.unlock();
  }
  else {
    child = node.children[bestChildIdx];

    while(child->statsLock.test_and_set(std::memory_order_acquire));
    child->virtualLosses += searchParams.numVirtualLossesPerThread;
    child->statsLock.clear(std::memory_order_release);

    //Unlock before making moves if the child already exists since we don't depend on it at this point
    lock.unlock();

    assert(thread.history.isLegal(thread.board,moveLoc,thread.pla));
    thread.history.makeBoardMoveAssumeLegal(thread.board,moveLoc,thread.pla,rootKoHashTable);
    thread.pla = getOpp(thread.pla);
  }

  //Recurse!
  playoutDescend(thread,*child,posesWithChildBuf,false,searchParams.numVirtualLossesPerThread);

  //Update this node stats
  updateStatsAfterPlayout(node,thread,virtualLossesToSubtract,isRoot);
}
