#include "core/global.h"
#include "core/config_parser.h"
#include "core/timer.h"
#include "dataio/sgf.h"
#include "search/asyncbot.h"
#include "program/setup.h"
#include "program/playutils.h"
#include "program/play.h"
#include "main.h"

#define TCLAP_NAMESTARTSTRING "-" //Use single dashes for all flags
#include <tclap/CmdLine.h>

using namespace std;

static const vector<string> knownCommands = {
  //Basic GTP commands
  "protocol_version",
  "name",
  "version",
  "known_command",
  "list_commands",
  "quit",

  //GTP extension - specify "boardsize X:Y" or "boardsize X Y" for non-square sizes
  //rectangular_boardsize is an alias for boardsize, intended to make it more evident that we have such support
  "boardsize",
  "rectangular_boardsize",

  "clear_board",
  "komi",
  "play",
  "undo",

  //GTP extension - specify rules
  "kata-get-rules",
  "kata-set-rule",
  "kata-set-rules",

  "genmove",
  "genmove_debug", //Prints additional info to stderr
  "search_debug", //Prints additional info to stderr, doesn't actually make the move

  //Clears neural net cached evaluations and bot search tree, allows fresh randomization
  "clear_cache",

  "showboard",
  "fixed_handicap",
  "place_free_handicap",
  "set_free_handicap",
  "time_settings",
  "time_left",
  "final_score",
  "final_status_list",

  "loadsgf",

  //GTP extensions for board analysis
  // "genmove_analyze",
  "lz-genmove_analyze",
  "kata-genmove_analyze",
  // "analyze",
  "lz-analyze",
  "kata-analyze",

  //Stop any ongoing ponder or analyze
  "stop",
};

static bool tryParseLoc(const string& s, const Board& b, Loc& loc) {
  return Location::tryOfString(s,b,loc);
}

static double initialBlackAdvantage(const BoardHistory& hist) {
  //Assume an advantage of 15 * number of black stones beyond the one black normally gets on the first move and komi
  int extraBlackStones = hist.computeNumHandicapStones();
  //Subtract one since white gets the first move afterward
  if(extraBlackStones > 0)
    extraBlackStones -= 1;
  return 15.0 * extraBlackStones + (7.0 - hist.rules.komi);
}

static bool noWhiteStonesOnBoard(const Board& board) {
  for(int y = 0; y < board.y_size; y++) {
    for(int x = 0; x < board.x_size; x++) {
      Loc loc = Location::getLoc(x,y,board.x_size);
      if(board.colors[loc] == P_WHITE)
        return false;
    }
  }
  return true;
}

static void updatePlayoutDoublingAdvantageHelper(
  AsyncBot* bot, const Board& board, const BoardHistory& hist, Player pla,
  const double dynamicPlayoutDoublingAdvantageCapPerOppLead,
  const vector<double>& recentWinLossValues,
  double& desiredPlayoutDoublingAdvantage,
  SearchParams& params
) {
  (void)board;
  if(dynamicPlayoutDoublingAdvantageCapPerOppLead <= 0.0)
    return;

  double pdaScalingStartPoints = 7.0;
  double initialBlackAdvantageInPoints = initialBlackAdvantage(hist);
  if(initialBlackAdvantageInPoints < pdaScalingStartPoints || pla != params.playoutDoublingAdvantagePla) {
    desiredPlayoutDoublingAdvantage = 0.0;
  }
  else {
    //What increment to adjust desiredPlayoutDoublingAdvantage at.
    //Power of 2 to avoid any rounding issues.
    const double increment = 0.125;

    //Hard cap of 2.5 in this parameter, since more extreme values start to reach into values without good training.
    //Scale mildly with board size - small board a given point lead counts as "more".
    double pdaCap = std::min(
      2.75,
      dynamicPlayoutDoublingAdvantageCapPerOppLead *
      (initialBlackAdvantageInPoints - pdaScalingStartPoints) * pow(19.0 * 19.0 / (double)(board.x_size * board.y_size), 0.25)
    );
    pdaCap = round(pdaCap / increment) * increment;

    //No history, or literally no white stones on board? Then this is a new game or a newly set position
    if(recentWinLossValues.size() <= 0 || noWhiteStonesOnBoard(board)) {
      //Just use the cap
      desiredPlayoutDoublingAdvantage = pdaCap;
    }
    else {
      double winLossValue = recentWinLossValues[recentWinLossValues.size()-1];
      if(pla == P_BLACK)
        winLossValue = -winLossValue;

      //Keep winLossValue between 5% and 25%, subject to available caps.
      if(winLossValue < -0.9)
        desiredPlayoutDoublingAdvantage = desiredPlayoutDoublingAdvantage + 0.125;
      else if(winLossValue > -0.5)
        desiredPlayoutDoublingAdvantage = desiredPlayoutDoublingAdvantage - 0.125;

      desiredPlayoutDoublingAdvantage = std::max(desiredPlayoutDoublingAdvantage, 0.0);
      desiredPlayoutDoublingAdvantage = std::min(desiredPlayoutDoublingAdvantage, pdaCap);
    }
  }

  if(params.playoutDoublingAdvantage != desiredPlayoutDoublingAdvantage) {
    params.playoutDoublingAdvantage = desiredPlayoutDoublingAdvantage;
    bot->setParams(params);
  }
}

static bool shouldResign(
  const Board& board,
  const BoardHistory& hist,
  Player pla,
  const vector<double>& recentWinLossValues,
  double lead,
  const double resignThreshold,
  const int resignConsecTurns
) {
  double initialBlackAdvantageInPoints = initialBlackAdvantage(hist);

  int minTurnForResignation = 0;
  double noResignationWhenWhiteScoreAbove = board.x_size * board.y_size;
  if(initialBlackAdvantageInPoints > 0.9 && pla == P_WHITE) {
    //Play at least some moves no matter what
    minTurnForResignation = 1 + board.x_size * board.y_size / 5;

    //In a handicap game, also only resign if the lead difference is well behind schedule assuming
    //that we're supposed to catch up over many moves.
    double numTurnsToCatchUp = 0.60 * board.x_size * board.y_size - minTurnForResignation;
    double numTurnsSpent = (double)(hist.moveHistory.size()) - minTurnForResignation;
    if(numTurnsToCatchUp <= 1.0)
      numTurnsToCatchUp = 1.0;
    if(numTurnsSpent <= 0.0)
      numTurnsSpent = 0.0;
    if(numTurnsSpent > numTurnsToCatchUp)
      numTurnsSpent = numTurnsToCatchUp;

    double resignScore = -initialBlackAdvantageInPoints * ((numTurnsToCatchUp - numTurnsSpent) / numTurnsToCatchUp);
    resignScore -= 5.0; //Always require at least a 5 point buffer
    resignScore -= initialBlackAdvantageInPoints * 0.15; //And also require a 15% of the initial handicap

    noResignationWhenWhiteScoreAbove = resignScore;
  }

  if(hist.moveHistory.size() < minTurnForResignation)
    return false;
  if(pla == P_WHITE && lead > noResignationWhenWhiteScoreAbove)
    return false;
  if(resignConsecTurns > recentWinLossValues.size())
    return false;

  for(int i = 0; i<resignConsecTurns; i++) {
    double winLossValue = recentWinLossValues[recentWinLossValues.size()-1-i];
    Player resignPlayerThisTurn = C_EMPTY;
    if(winLossValue < resignThreshold)
      resignPlayerThisTurn = P_WHITE;
    else if(winLossValue > -resignThreshold)
      resignPlayerThisTurn = P_BLACK;

    if(resignPlayerThisTurn != pla)
      return false;
  }

  return true;
}

static void printGenmoveLog(ostream& out, const AsyncBot* bot, const NNEvaluator* nnEval, Loc moveLoc, double timeTaken, Player perspective) {
  const Search* search = bot->getSearch();
  //Board::printBoard(out, bot->getRootBoard(), moveLoc, &(bot->getRootHist().moveHistory));
  out << bot->getRootHist().rules << "\n";
  out << "Time taken: " << timeTaken << "\n";
  out << "Root visits: " << search->numRootVisits() << "\n";
  out << "NN rows: " << nnEval->numRowsProcessed() << endl;
  out << "NN batches: " << nnEval->numBatchesProcessed() << endl;
  out << "NN avg batch size: " << nnEval->averageProcessedBatchSize() << endl;
  if(search->searchParams.playoutDoublingAdvantage != 0)
    out << "PlayoutDoublingAdvantage: " << search->searchParams.playoutDoublingAdvantage << endl;
  out << "PV: ";
  search->printPV(out, search->rootNode, 25);
  out << "\n";
  out << "Tree:\n";
  search->printTree(out, search->rootNode, PrintTreeOptions().maxDepth(1).maxChildrenToShow(10),perspective);
}

struct GTPEngine {
  GTPEngine(const GTPEngine&) = delete;
  GTPEngine& operator=(const GTPEngine&) = delete;

  const string nnModelFile;
  const bool assumeMultipleStartingBlackMovesAreHandicap;
  const int analysisPVLen;
  const bool preventEncore;
  const double dynamicPlayoutDoublingAdvantageCapPerOppLead;

  NNEvaluator* nnEval;
  AsyncBot* bot;
  Rules currentRules; //Should always be the same as the rules in bot, if bot is not NULL.

  SearchParams params;
  TimeControls bTimeControls;
  TimeControls wTimeControls;

  //This move history doesn't get cleared upon consecutive moves by the same side, and is used
  //for undo, whereas the one in search does.
  Board initialBoard;
  Player initialPla;
  vector<Move> moveHistory;

  vector<double> recentWinLossValues;
  double lastSearchFactor;
  double desiredPlayoutDoublingAdvantage;

  Player perspective;

  GTPEngine(
    const string& modelFile, SearchParams initialParams, Rules initialRules,
    bool assumeMultiBlackHandicap, bool prevtEncore, double dynamicPDACapPerOppLead,
    Player persp, int pvLen
  )
    :nnModelFile(modelFile),
     assumeMultipleStartingBlackMovesAreHandicap(assumeMultiBlackHandicap),
     analysisPVLen(pvLen),
     preventEncore(prevtEncore),
     dynamicPlayoutDoublingAdvantageCapPerOppLead(dynamicPDACapPerOppLead),
     nnEval(NULL),
     bot(NULL),
     currentRules(initialRules),
     params(initialParams),
     bTimeControls(),
     wTimeControls(),
     initialBoard(),
     initialPla(P_BLACK),
     moveHistory(),
     recentWinLossValues(),
     lastSearchFactor(1.0),
     perspective(persp)
  {
  }

  ~GTPEngine() {
    stopAndWait();
    delete bot;
    delete nnEval;
  }

  void stopAndWait() {
    bot->stopAndWait();
  }

  Rules getCurrentRules() {
    return currentRules;
  }

  //Specify -1 for the sizes for a default
  void setOrResetBoardSize(ConfigParser& cfg, Logger& logger, Rand& seedRand, int boardXSize, int boardYSize) {
    if(nnEval != NULL && boardXSize == nnEval->getNNXLen() && boardYSize == nnEval->getNNYLen())
      return;
    if(nnEval != NULL) {
      assert(bot != NULL);
      bot->stopAndWait();
      delete bot;
      delete nnEval;
      bot = NULL;
      nnEval = NULL;
      logger.write("Cleaned up old neural net and bot");
    }

    //Initial setup
    bool wasDefault = false;
    if(boardXSize == -1 || boardYSize == -1) {
      boardXSize = 19;
      boardYSize = 19;
      wasDefault = true;
    }

    int maxConcurrentEvals = params.numThreads * 2 + 16; // * 2 + 16 just to give plenty of headroom
    int defaultMaxBatchSize = std::max(8,((params.numThreads+3)/4)*4);
    nnEval = Setup::initializeNNEvaluator(
      nnModelFile,nnModelFile,cfg,logger,seedRand,maxConcurrentEvals,
      boardXSize,boardYSize,defaultMaxBatchSize,
      Setup::SETUP_FOR_GTP
    );
    logger.write("Loaded neural net with nnXLen " + Global::intToString(nnEval->getNNXLen()) + " nnYLen " + Global::intToString(nnEval->getNNYLen()));

    {
      bool rulesWereSupported;
      nnEval->getSupportedRules(currentRules,rulesWereSupported);
      if(!rulesWereSupported) {
        throw StringError("Rules " + currentRules.toJsonStringNoKomi() + " from config file " + cfg.getFileName() + " are NOT supported by neural net");
      }
    }

    //On initial setup, size the board to whatever the neural net was initialized with
    //So that if the net was initalized smaller, we don't fail with a big board
    if(wasDefault) {
      boardXSize = nnEval->getNNXLen();
      boardYSize = nnEval->getNNYLen();
    }

    string searchRandSeed;
    if(cfg.contains("searchRandSeed"))
      searchRandSeed = cfg.getString("searchRandSeed");
    else
      searchRandSeed = Global::uint64ToString(seedRand.nextUInt64());

    bot = new AsyncBot(params, nnEval, &logger, searchRandSeed);

    Board board(boardXSize,boardYSize);
    Player pla = P_BLACK;
    BoardHistory hist(board,pla,currentRules,0);
    vector<Move> newMoveHistory;
    setPositionAndRules(pla,board,hist,board,pla,newMoveHistory);
  }

  void setPositionAndRules(Player pla, const Board& board, const BoardHistory& h, const Board& newInitialBoard, Player newInitialPla, const vector<Move> newMoveHistory) {
    BoardHistory hist(h);
    //Ensure we always have this value correct
    hist.setAssumeMultipleStartingBlackMovesAreHandicap(assumeMultipleStartingBlackMovesAreHandicap);

    currentRules = hist.rules;
    bot->setPosition(pla,board,hist);
    initialBoard = newInitialBoard;
    initialPla = newInitialPla;
    moveHistory = newMoveHistory;
    recentWinLossValues.clear();
    //Update it assuming the bot will be playing as params.playoutDoublingAdvantagePla
    updatePlayoutDoublingAdvantage(params.playoutDoublingAdvantagePla);
  }

  void clearBoard() {
    assert(bot->getRootHist().rules == currentRules);
    int newXSize = bot->getRootBoard().x_size;
    int newYSize = bot->getRootBoard().y_size;
    Board board(newXSize,newYSize);
    Player pla = P_BLACK;
    BoardHistory hist(board,pla,currentRules,0);
    vector<Move> newMoveHistory;
    setPositionAndRules(pla,board,hist,board,pla,newMoveHistory);
  }

  void updateKomiIfNew(float newKomi) {
    bot->setKomiIfNew(newKomi);
    currentRules.komi = newKomi;
  }

  //Update playout doubling advantage for the engine for playing as pla
  void updatePlayoutDoublingAdvantage(Player pla) {
    updatePlayoutDoublingAdvantageHelper(
      bot,bot->getRootBoard(),bot->getRootHist(),pla,
      dynamicPlayoutDoublingAdvantageCapPerOppLead,
      recentWinLossValues,
      desiredPlayoutDoublingAdvantage,params
    );
  }

  bool play(Loc loc, Player pla) {
    assert(bot->getRootHist().rules == currentRules);
    bool suc = bot->makeMove(loc,pla,preventEncore);
    if(suc)
      moveHistory.push_back(Move(loc,pla));
    return suc;
  }

  bool undo() {
    if(moveHistory.size() <= 0)
      return false;
    assert(bot->getRootHist().rules == currentRules);

    vector<Move> moveHistoryCopy = moveHistory;

    Board undoneBoard = initialBoard;
    BoardHistory undoneHist(undoneBoard,initialPla,currentRules,0);
    vector<Move> emptyMoveHistory;
    setPositionAndRules(initialPla,undoneBoard,undoneHist,initialBoard,initialPla,emptyMoveHistory);

    for(int i = 0; i<moveHistoryCopy.size()-1; i++) {
      Loc moveLoc = moveHistoryCopy[i].loc;
      Player movePla = moveHistoryCopy[i].pla;
      bool suc = play(moveLoc,movePla);
      assert(suc);
      (void)suc; //Avoid warning when asserts are off
    }
    return true;
  }

  bool setRulesNotIncludingKomi(Rules newRules, string& error) {
    assert(nnEval != NULL);
    assert(bot->getRootHist().rules == currentRules);
    newRules.komi = currentRules.komi;

    bool rulesWereSupported;
    nnEval->getSupportedRules(newRules,rulesWereSupported);
    if(!rulesWereSupported) {
      error = "Rules " + newRules.toJsonStringNoKomi() + " are not supported by this neural net version";
      return false;
    }

    vector<Move> moveHistoryCopy = moveHistory;

    Board board = initialBoard;
    BoardHistory hist(board,initialPla,newRules,0);
    vector<Move> emptyMoveHistory;
    setPositionAndRules(initialPla,board,hist,initialBoard,initialPla,emptyMoveHistory);

    for(int i = 0; i<moveHistoryCopy.size(); i++) {
      Loc moveLoc = moveHistoryCopy[i].loc;
      Player movePla = moveHistoryCopy[i].pla;
      bool suc = play(moveLoc,movePla);

      //Because internally we use a highly tolerant test, we don't expect this to actually trigger
      //even if a rules change did make some earlier moves illegal. But this check simply futureproofs
      //things in case we ever do
      if(!suc) {
        error = "Could not make the rules change, some earlier moves in the game would now become illegal.";
        return false;
      }
    }
    return true;
  }

  void ponder() {
    bot->ponder(lastSearchFactor);
  }

  struct AnalyzeArgs {
    bool analyzing = false;
    bool lz = false;
    bool kata = false;
    int minMoves = 0;
    int maxMoves = 10000000;
    bool showOwnership = false;
    double secondsPerReport = 1e30;
  };

  std::function<void(const Search* search)> getAnalyzeCallback(Player pla, AnalyzeArgs args) {
    std::function<void(const Search* search)> callback;
    //analyze
    if(!args.kata && !args.lz) {
      callback = [args,pla,this](const Search* search) {
        vector<AnalysisData> buf;
        search->getAnalysisData(buf,args.minMoves,false,analysisPVLen);
        if(buf.size() > args.maxMoves)
          buf.resize(args.maxMoves);
        if(buf.size() <= 0)
          return;

        const Board board = search->getRootBoard();
        for(int i = 0; i<buf.size(); i++) {
          if(i > 0)
            cout << " ";
          const AnalysisData& data = buf[i];
          double winrate = 0.5 * (1.0 + data.winLossValue);
          if(perspective == P_BLACK || (perspective != P_BLACK && perspective != P_WHITE && pla == P_BLACK)) {
            winrate = 1.0-winrate;
          }
          cout << "info";
          cout << " move " << Location::toString(data.move,board);
          cout << " visits " << data.numVisits;
          cout << " winrate " << round(winrate * 10000.0);
          cout << " order " << data.order;
        }
        cout << endl;
      };
    }
    //lz-analyze
    else if(!args.kata) {
      callback = [args,pla,this](const Search* search) {
        vector<AnalysisData> buf;
        search->getAnalysisData(buf,args.minMoves,false,analysisPVLen);
        if(buf.size() > args.maxMoves)
          buf.resize(args.maxMoves);
        if(buf.size() <= 0)
          return;

        const Board board = search->getRootBoard();
        for(int i = 0; i<buf.size(); i++) {
          if(i > 0)
            cout << " ";
          const AnalysisData& data = buf[i];
          double winrate = 0.5 * (1.0 + data.winLossValue);
          double lcb = PlayUtils::getHackedLCBForWinrate(search,data,pla);
          if(perspective == P_BLACK || (perspective != P_BLACK && perspective != P_WHITE && pla == P_BLACK)) {
            winrate = 1.0-winrate;
            lcb = 1.0 - lcb;
          }
          cout << "info";
          cout << " move " << Location::toString(data.move,board);
          cout << " visits " << data.numVisits;
          cout << " winrate " << round(winrate * 10000.0);
          cout << " prior " << round(data.policyPrior * 10000.0);
          cout << " lcb " << round(lcb * 10000.0);
          cout << " order " << data.order;
          cout << " pv ";
          if(preventEncore && data.pvContainsPass())
            data.writePVUpToPhaseEnd(cout,board,search->getRootHist(),search->getRootPla());
          else
            data.writePV(cout,board);
        }
        cout << endl;
      };
    }
    //kata-analyze
    else {
      callback = [args,pla,this](const Search* search) {
        vector<AnalysisData> buf;
        search->getAnalysisData(buf,args.minMoves,false,analysisPVLen);
        if(buf.size() > args.maxMoves)
          buf.resize(args.maxMoves);
        if(buf.size() <= 0)
          return;

        vector<double> ownership;
        if(args.showOwnership) {
          static constexpr int64_t ownershipMinVisits = 3;
          ownership = search->getAverageTreeOwnership(ownershipMinVisits);
        }

        const Board board = search->getRootBoard();
        for(int i = 0; i<buf.size(); i++) {
          if(i > 0)
            cout << " ";
          const AnalysisData& data = buf[i];
          double winrate = 0.5 * (1.0 + data.winLossValue);
          double utility = data.utility;
          //We still hack the LCB for consistency with LZ-analyze
          double lcb = PlayUtils::getHackedLCBForWinrate(search,data,pla);
          ///But now we also offer the proper LCB that KataGo actually uses.
          double utilityLcb = data.lcb;
          double scoreMean = data.scoreMean;
          double lead = data.lead;
          if(perspective == P_BLACK || (perspective != P_BLACK && perspective != P_WHITE && pla == P_BLACK)) {
            winrate = 1.0-winrate;
            lcb = 1.0 - lcb;
            utility = -utility;
            scoreMean = -scoreMean;
            lead = -lead;
            utilityLcb = -utilityLcb;
          }
          cout << "info";
          cout << " move " << Location::toString(data.move,board);
          cout << " visits " << data.numVisits;
          cout << " utility " << utility;
          cout << " winrate " << winrate;
          cout << " scoreMean " << lead;
          cout << " scoreStdev " << data.scoreStdev;
          cout << " scoreLead " << lead;
          cout << " scoreSelfplay " << scoreMean;
          cout << " prior " << data.policyPrior;
          cout << " lcb " << lcb;
          cout << " utilityLcb " << utilityLcb;
          cout << " order " << data.order;
          cout << " pv ";
          if(preventEncore && data.pvContainsPass())
            data.writePVUpToPhaseEnd(cout,board,search->getRootHist(),search->getRootPla());
          else
            data.writePV(cout,board);
        }

        if(args.showOwnership) {
          cout << " ";

          cout << "ownership";
          int nnXLen = search->nnXLen;
          for(int y = 0; y<board.y_size; y++) {
            for(int x = 0; x<board.x_size; x++) {
              int pos = NNPos::xyToPos(x,y,nnXLen);
              if(perspective == P_BLACK || (perspective != P_BLACK && perspective != P_WHITE && pla == P_BLACK))
                cout << " " << -ownership[pos];
              else
                cout << " " << ownership[pos];
            }
          }
        }

        cout << endl;
      };
    }
    return callback;
  }

  void genMove(
    Player pla,
    Logger& logger, double searchFactorWhenWinningThreshold, double searchFactorWhenWinning,
    bool cleanupBeforePass, bool ogsChatToStderr,
    bool allowResignation, double resignThreshold, int resignConsecTurns,
    bool logSearchInfo, bool debug, bool playChosenMove,
    string& response, bool& responseIsError, bool& maybeStartPondering,
    AnalyzeArgs args
  ) {
    response = "";
    responseIsError = false;
    maybeStartPondering = false;

    ClockTimer timer;
    nnEval->clearStats();
    TimeControls tc = pla == P_BLACK ? bTimeControls : wTimeControls;

    //Update PDA given whatever the most recent values are
    updatePlayoutDoublingAdvantage(pla);
    //Make sure we have the right PDA parameters, in case someone ran analysis in the meantime.
    if(dynamicPlayoutDoublingAdvantageCapPerOppLead != 0.0 &&
       params.playoutDoublingAdvantage != desiredPlayoutDoublingAdvantage) {
      params.playoutDoublingAdvantage = desiredPlayoutDoublingAdvantage;
      bot->setParams(params);
    }

    //Play faster when winning
    double searchFactor = PlayUtils::getSearchFactor(searchFactorWhenWinningThreshold,searchFactorWhenWinning,params,recentWinLossValues,pla);
    lastSearchFactor = searchFactor;

    Loc moveLoc;
    if(args.analyzing) {
      std::function<void(const Search* search)> callback = getAnalyzeCallback(pla,args);
      if(args.showOwnership)
        bot->setAlwaysIncludeOwnerMap(true);
      else
        bot->setAlwaysIncludeOwnerMap(false);
      moveLoc = bot->genMoveSynchronousAnalyze(pla, tc, searchFactor, args.secondsPerReport, callback);
      //Make sure callback happens at least once
      callback(bot->getSearch());
    }
    else {
      moveLoc = bot->genMoveSynchronous(pla,tc,searchFactor);
    }

    bool isLegal = bot->isLegalStrict(moveLoc,pla);
    if(moveLoc == Board::NULL_LOC || !isLegal) {
      responseIsError = true;
      response = "genmove returned null location or illegal move";
      ostringstream sout;
      sout << "genmove null location or illegal move!?!" << "\n";
      sout << bot->getRootBoard() << "\n";
      sout << "Pla: " << PlayerIO::playerToString(pla) << "\n";
      sout << "MoveLoc: " << Location::toString(moveLoc,bot->getRootBoard()) << "\n";
      logger.write(sout.str());
      return;
    }

    //Implement cleanupBeforePass hack - the bot wants to pass, so instead cleanup if there is something to clean
    //Make sure we only do it though when it makes sense to do so.
    if(cleanupBeforePass && moveLoc == Board::PASS_LOC && bot->getRootHist().isFinalPhase()) {
      Board board = bot->getRootBoard();
      BoardHistory hist = bot->getRootHist();
      Color* safeArea = bot->getSearch()->rootSafeArea;
      assert(safeArea != NULL);
      //Scan the board for any spot that is adjacent to an opponent group that is part of our pass-alive territory.
      for(int y = 0; y<board.y_size; y++) {
        for(int x = 0; x<board.x_size; x++) {
          Loc otherLoc = Location::getLoc(x,y,board.x_size);
          if(moveLoc == Board::PASS_LOC &&
             board.colors[otherLoc] == C_EMPTY &&
             safeArea[otherLoc] == pla &&
             board.isAdjacentToPla(otherLoc,getOpp(pla)) &&
             hist.isLegal(board,otherLoc,pla)
          ) {
            moveLoc = otherLoc;
          }
        }
      }
    }

    ReportedSearchValues values;
    double winLossValue;
    double lead;
    {
      values = bot->getSearch()->getRootValuesAssertSuccess();
      winLossValue = values.winLossValue;
      lead = values.lead;
    }

    double timeTaken = timer.getSeconds();

    //Chatting and logging ----------------------------

    if(ogsChatToStderr) {
      int64_t visits = bot->getSearch()->getRootVisits();
      double winrate = 0.5 * (1.0 + (values.winValue - values.lossValue));
      double leadForPrinting = lead;
      //Print winrate from desired perspective
      if(perspective == P_BLACK || (perspective != P_BLACK && perspective != P_WHITE && pla == P_BLACK)) {
        winrate = 1.0 - winrate;
        leadForPrinting = -leadForPrinting;
      }
      cerr << "CHAT:"
           << "Visits " << visits
           << " Winrate " << Global::strprintf("%.2f%%", winrate * 100.0)
           << " ScoreLead " << Global::strprintf("%.1f", leadForPrinting)
           << " ScoreStdev " << Global::strprintf("%.1f", values.expectedScoreStdev);
      if(params.playoutDoublingAdvantage != 0.0)
        cerr << Global::strprintf(" (PDA %.2f)", params.playoutDoublingAdvantage);
      cerr << " PV ";
      bot->getSearch()->printPVForMove(cerr,bot->getSearch()->rootNode, moveLoc, analysisPVLen);
      cerr << endl;
    }

    if(logSearchInfo) {
      ostringstream sout;
      printGenmoveLog(sout,bot,nnEval,moveLoc,timeTaken,perspective);
      logger.write(sout.str());
    }
    if(debug) {
      printGenmoveLog(cerr,bot,nnEval,moveLoc,timeTaken,perspective);
    }

    //Adjust handicap games ------------------------
    recentWinLossValues.push_back(winLossValue);

    //Resignation, actual reporting of chosen move---------------------

    bool resigned = allowResignation && shouldResign(
      bot->getRootBoard(),bot->getRootHist(),pla,recentWinLossValues,lead,
      resignThreshold,resignConsecTurns
    );

    if(resigned)
      response = "resign";
    else
      response = Location::toString(moveLoc,bot->getRootBoard());

    if(!resigned && moveLoc != Board::NULL_LOC && isLegal && playChosenMove) {
      bool suc = bot->makeMove(moveLoc,pla,preventEncore);
      if(suc)
        moveHistory.push_back(Move(moveLoc,pla));
      assert(suc);
      (void)suc; //Avoid warning when asserts are off

      maybeStartPondering = true;
    }

    if(args.analyzing) {
      response = "play " + response;
    }
    return;
  }

  void clearCache() {
    bot->clearSearch();
    nnEval->clearCache();
  }

  void placeFixedHandicap(int n, string& response, bool& responseIsError) {
    int xSize = bot->getRootBoard().x_size;
    int ySize = bot->getRootBoard().y_size;
    Board board(xSize,ySize);
    try {
      PlayUtils::placeFixedHandicap(board,n);
    }
    catch(const StringError& e) {
      responseIsError = true;
      response = string(e.what()) + ", try place_free_handicap";
      return;
    }
    assert(bot->getRootHist().rules == currentRules);

    Player pla = P_BLACK;
    BoardHistory hist(board,pla,currentRules,0);

    //Also switch the initial player, expecting white should be next.
    hist.clear(board,P_WHITE,currentRules,0);
    hist.setAssumeMultipleStartingBlackMovesAreHandicap(assumeMultipleStartingBlackMovesAreHandicap);
    pla = P_WHITE;

    response = "";
    for(int y = 0; y<board.y_size; y++) {
      for(int x = 0; x<board.x_size; x++) {
        Loc loc = Location::getLoc(x,y,board.x_size);
        if(board.colors[loc] != C_EMPTY) {
          response += " " + Location::toString(loc,board);
        }
      }
    }
    response = Global::trim(response);
    (void)responseIsError;

    vector<Move> newMoveHistory;
    setPositionAndRules(pla,board,hist,board,pla,newMoveHistory);
  }

  void placeFreeHandicap(int n, string& response, bool& responseIsError) {
    stopAndWait();

    //If asked to place more, we just go ahead and only place up to 30, or a quarter of the board
    int xSize = bot->getRootBoard().x_size;
    int ySize = bot->getRootBoard().y_size;
    int maxHandicap = xSize*ySize / 4;
    if(maxHandicap > 30)
      maxHandicap = 30;
    if(n > maxHandicap)
      n = maxHandicap;

    assert(bot->getRootHist().rules == currentRules);

    Board board(xSize,ySize);
    Player pla = P_BLACK;
    BoardHistory hist(board,pla,currentRules,0);
    double extraBlackTemperature = 0.25;
    Rand rand;
    PlayUtils::playExtraBlack(bot->getSearchStopAndWait(), n, board, hist, extraBlackTemperature, rand);
    //Also switch the initial player, expecting white should be next.
    hist.clear(board,P_WHITE,currentRules,0);
    hist.setAssumeMultipleStartingBlackMovesAreHandicap(assumeMultipleStartingBlackMovesAreHandicap);
    pla = P_WHITE;

    response = "";
    for(int y = 0; y<board.y_size; y++) {
      for(int x = 0; x<board.x_size; x++) {
        Loc loc = Location::getLoc(x,y,board.x_size);
        if(board.colors[loc] != C_EMPTY) {
          response += " " + Location::toString(loc,board);
        }
      }
    }
    response = Global::trim(response);
    (void)responseIsError;

    vector<Move> newMoveHistory;
    setPositionAndRules(pla,board,hist,board,pla,newMoveHistory);
  }

  void analyze(Player pla, AnalyzeArgs args) {
    assert(args.analyzing);
    //In dynamic mode, analysis should ALWAYS be with 0.0, to prevent random hard-to-predict changes
    //for users.
    if(dynamicPlayoutDoublingAdvantageCapPerOppLead != 0.0 && params.playoutDoublingAdvantage != 0.0) {
      params.playoutDoublingAdvantage = 0.0;
      bot->setParams(params);
    }

    std::function<void(Search* search)> callback = getAnalyzeCallback(pla,args);
    if(args.showOwnership)
      bot->setAlwaysIncludeOwnerMap(true);
    else
      bot->setAlwaysIncludeOwnerMap(false);

    double searchFactor = 1e40; //go basically forever
    bot->analyze(pla, searchFactor, args.secondsPerReport, callback);
  }

  double computeLead(Logger& logger) {
    stopAndWait();

    //Make absolutely sure we can restore the bot's old state
    const Player oldPla = bot->getRootPla();
    const Board oldBoard = bot->getRootBoard();
    const BoardHistory oldHist = bot->getRootHist();

    Board board = bot->getRootBoard();
    BoardHistory hist = bot->getRootHist();
    Player pla = bot->getRootPla();

    int64_t numVisits = std::max(50, params.numThreads * 10);
    //Try computing the lead for white
    double lead = PlayUtils::computeLead(bot->getSearchStopAndWait(),NULL,board,hist,pla,numVisits,logger,OtherGameProperties());

    //Restore
    bot->setPosition(oldPla,oldBoard,oldHist);

    //Round lead to nearest integer or half-integer
    if(hist.rules.gameResultWillBeInteger())
      lead = round(lead);
    else
      lead = round(lead+0.5)-0.5;

    return lead;
  }

  vector<bool> computeAnticipatedStatusesWithOwnership(Logger& logger) {
    stopAndWait();

    //Make absolutely sure we can restore the bot's old state
    const Player oldPla = bot->getRootPla();
    const Board oldBoard = bot->getRootBoard();
    const BoardHistory oldHist = bot->getRootHist();

    Board board = bot->getRootBoard();
    BoardHistory hist = bot->getRootHist();
    Player pla = bot->getRootPla();

    int64_t numVisits = std::max(100, params.numThreads * 20);
    vector<bool> isAlive = PlayUtils::computeAnticipatedStatusesWithOwnership(bot->getSearchStopAndWait(),board,hist,pla,numVisits,logger);

    //Restore
    bot->setPosition(oldPla,oldBoard,oldHist);

    return isAlive;
  }

};


//User should pre-fill pla with a default value, as it will not get filled in if the parsed command doesn't specify
static GTPEngine::AnalyzeArgs parseAnalyzeCommand(const string& command, const vector<string>& pieces, Player& pla, bool& parseFailed) {
  int numArgsParsed = 0;

  bool isLZ = (command == "lz-analyze" || command == "lz-genmove_analyze");
  bool isKata = (command == "kata-analyze" || command == "kata-genmove_analyze");
  double lzAnalyzeInterval = 1e30;
  int minMoves = 0;
  int maxMoves = 10000000;
  bool showOwnership = false;

  parseFailed = false;

  //Format:
  //lz-analyze [optional player] [optional interval float] <keys and values>
  //Keys and values consists of zero or more of:

  //interval <float interval in centiseconds>
  //avoid <player> <comma-separated moves> <until movenum>
  //minmoves <int min number of moves to show>
  //maxmoves <int max number of moves to show>
  //ownership <bool whether to show ownership or not>

  //Parse optional player
  if(pieces.size() > numArgsParsed && PlayerIO::tryParsePlayer(pieces[numArgsParsed],pla))
    numArgsParsed += 1;

  //Parse optional interval float
  if(pieces.size() > numArgsParsed &&
     Global::tryStringToDouble(pieces[numArgsParsed],lzAnalyzeInterval) &&
     !isnan(lzAnalyzeInterval) && lzAnalyzeInterval >= 0 && lzAnalyzeInterval < 1e20)
    numArgsParsed += 1;

  //Now loop and handle all key value pairs
  while(pieces.size() > numArgsParsed) {
    const string& key = pieces[numArgsParsed];
    numArgsParsed += 1;
    //Make sure we have a value. If not, then we fail.
    if(pieces.size() <= numArgsParsed) {
      parseFailed = true;
      break;
    }

    const string& value = pieces[numArgsParsed];
    numArgsParsed += 1;

    if(key == "interval" && Global::tryStringToDouble(value,lzAnalyzeInterval) &&
       !isnan(lzAnalyzeInterval) && lzAnalyzeInterval >= 0 && lzAnalyzeInterval < 1e20) {
      continue;
    }
    //Parse it but ignore it since we don't support excluding moves right now
    else if(key == "avoid" || key == "allow") {
      //Parse two more arguments, and ignore them
      if(pieces.size() <= numArgsParsed+1) {
        parseFailed = true;
        break;
      }
      const string& moves = pieces[numArgsParsed];
      (void)moves;
      numArgsParsed += 1;
      const string& untilMove = pieces[numArgsParsed];
      (void)untilMove;
      numArgsParsed += 1;
      continue;
    }
    else if(key == "minmoves" && Global::tryStringToInt(value,minMoves) &&
            minMoves >= 0 && minMoves < 1000000000) {
      continue;
    }
    else if(key == "maxmoves" && Global::tryStringToInt(value,maxMoves) &&
            maxMoves >= 0 && maxMoves < 1000000000) {
      continue;
    }
    else if(isKata && key == "ownership" && Global::tryStringToBool(value,showOwnership)) {
      continue;
    }

    parseFailed = true;
    break;
  }

  GTPEngine::AnalyzeArgs args = GTPEngine::AnalyzeArgs();
  args.analyzing = true;
  args.lz = isLZ;
  args.kata = isKata;
  //Convert from centiseconds to seconds
  args.secondsPerReport = lzAnalyzeInterval * 0.01;
  args.minMoves = minMoves;
  args.maxMoves = maxMoves;
  args.showOwnership = showOwnership;
  return args;
}


int MainCmds::gtp(int argc, const char* const* argv) {
  Board::initHash();
  ScoreValue::initTables();
  Rand seedRand;

  string configFile;
  string nnModelFile;
  string overrideVersion;
  try {
    TCLAP::CmdLine cmd("Run GTP engine", ' ', Version::getKataGoVersionForHelp(),true);
    TCLAP::ValueArg<string> configFileArg("","config","Config file to use (see configs/gtp_example.cfg)",true,string(),"FILE");
    TCLAP::ValueArg<string> nnModelFileArg("","model","Neural net model file",true,string(),"FILE");
    TCLAP::ValueArg<string> overrideVersionArg("","override-version","Force KataGo to say a certain value in response to gtp version command",false,string(),"VERSION");
    cmd.add(configFileArg);
    cmd.add(nnModelFileArg);
    cmd.add(overrideVersionArg);
    cmd.parse(argc,argv);
    configFile = configFileArg.getValue();
    nnModelFile = nnModelFileArg.getValue();
    overrideVersion = overrideVersionArg.getValue();
  }
  catch (TCLAP::ArgException &e) {
    cerr << "Error: " << e.error() << " for argument " << e.argId() << endl;
    return 1;
  }

  ConfigParser cfg(configFile);

  Logger logger;
  logger.addFile(cfg.getString("logFile"));
  bool logAllGTPCommunication = cfg.getBool("logAllGTPCommunication");
  bool logSearchInfo = cfg.getBool("logSearchInfo");
  bool loggingToStderr = false;

  bool startupPrintMessageToStderr = true;
  if(cfg.contains("startupPrintMessageToStderr"))
    startupPrintMessageToStderr = cfg.getBool("startupPrintMessageToStderr");

  if(cfg.contains("logToStderr") && cfg.getBool("logToStderr")) {
    loggingToStderr = true;
    logger.setLogToStderr(true);
  }

  logger.write("GTP Engine starting...");
  logger.write(Version::getKataGoVersionForHelp());
  //Also check loggingToStderr so that we don't duplicate the message from the log file
  if(startupPrintMessageToStderr && !loggingToStderr) {
    cerr << Version::getKataGoVersionForHelp() << endl;
  }

  //Defaults to 7.5 komi, gtp will generally override this
  Rules initialRules = Setup::loadSingleRulesExceptForKomi(cfg);

  SearchParams params = Setup::loadSingleParams(cfg);
  logger.write("Using " + Global::intToString(params.numThreads) + " CPU thread(s) for search");
  //Set a default for conservativePass that differs from matches or selfplay
  if(!cfg.contains("conservativePass") && !cfg.contains("conservativePass0"))
    params.conservativePass = true;
  if(!cfg.contains("fillDameBeforePass") && !cfg.contains("fillDameBeforePass0"))
    params.fillDameBeforePass = true;

  const bool ponderingEnabled = cfg.getBool("ponderingEnabled");
  const bool cleanupBeforePass = cfg.contains("cleanupBeforePass") ? cfg.getBool("cleanupBeforePass") : true;
  const bool allowResignation = cfg.contains("allowResignation") ? cfg.getBool("allowResignation") : false;
  const double resignThreshold = cfg.contains("allowResignation") ? cfg.getDouble("resignThreshold",-1.0,0.0) : -1.0; //Threshold on [-1,1], regardless of winLossUtilityFactor
  const int resignConsecTurns = cfg.contains("resignConsecTurns") ? cfg.getInt("resignConsecTurns",1,100) : 3;

  Setup::initializeSession(cfg);

  const double searchFactorWhenWinning = cfg.contains("searchFactorWhenWinning") ? cfg.getDouble("searchFactorWhenWinning",0.01,1.0) : 1.0;
  const double searchFactorWhenWinningThreshold = cfg.contains("searchFactorWhenWinningThreshold") ? cfg.getDouble("searchFactorWhenWinningThreshold",0.0,1.0) : 1.0;
  const bool ogsChatToStderr = cfg.contains("ogsChatToStderr") ? cfg.getBool("ogsChatToStderr") : false;
  const int analysisPVLen = cfg.contains("analysisPVLen") ? cfg.getInt("analysisPVLen",1,100) : 9;
  const bool assumeMultipleStartingBlackMovesAreHandicap =
    cfg.contains("assumeMultipleStartingBlackMovesAreHandicap") ? cfg.getBool("assumeMultipleStartingBlackMovesAreHandicap") : true;
  const bool preventEncore = cfg.contains("preventCleanupPhase") ? cfg.getBool("preventCleanupPhase") : true;
  const double dynamicPlayoutDoublingAdvantageCapPerOppLead =
    cfg.contains("dynamicPlayoutDoublingAdvantageCapPerOppLead") ? cfg.getDouble("dynamicPlayoutDoublingAdvantageCapPerOppLead",0.0,0.5) : 0.0;
  if(cfg.contains("dynamicPlayoutDoublingAdvantageCapPerOppLead") && (cfg.contains("playoutDoublingAdvantage") || cfg.contains("playoutDoublingAdvantage0")))
    throw StringError("Cannot specify both dynamicPlayoutDoublingAdvantageCapPerOppLead and playoutDoublingAdvantage");
  if(cfg.contains("dynamicPlayoutDoublingAdvantageCapPerOppLead") && params.playoutDoublingAdvantagePla == C_EMPTY)
    throw StringError("When specifying dynamicPlayoutDoublingAdvantageCapPerOppLead, must specify a player for playoutDoublingAdvantagePla");

  Player perspective = Setup::parseReportAnalysisWinrates(cfg,C_EMPTY);

  GTPEngine* engine = new GTPEngine(
    nnModelFile,params,initialRules,
    assumeMultipleStartingBlackMovesAreHandicap,preventEncore,dynamicPlayoutDoublingAdvantageCapPerOppLead,
    perspective,analysisPVLen
  );
  engine->setOrResetBoardSize(cfg,logger,seedRand,-1,-1);

  //Check for unused config keys
  cfg.warnUnusedKeys(cerr,&logger);

  logger.write("Loaded model "+ nnModelFile);
  logger.write("GTP ready, beginning main protocol loop");
  //Also check loggingToStderr so that we don't duplicate the message from the log file
  if(startupPrintMessageToStderr && !loggingToStderr) {
    cerr << "Loaded model " << nnModelFile << endl;
    cerr << "GTP ready, beginning main protocol loop" << endl;
  }

  bool currentlyAnalyzing = false;
  //gomoku
  string line = string{};
  while(1) {
    if(!getline(cin,line)){
      cout << endl;
      break;
    }

    //Parse command, extracting out the command itself, the arguments, and any GTP id number for the command.
    string command;
    vector<string> pieces;
    bool hasId = false;
    int id = 0;
    {
      //Filter down to only "normal" ascii characters. Also excludes carrage returns.
      //Newlines are already handled by getline
      size_t newLen = 0;
      for(size_t i = 0; i < line.length(); i++)
        if(((int)line[i] >= 32 && (int)line[i] <= 126) || line[i] == '\t')
          line[newLen++] = line[i];

      line.erase(line.begin()+newLen, line.end());

      //Remove comments
      size_t commentPos = line.find("#");
      if(commentPos != string::npos)
        line = line.substr(0, commentPos);

      //Convert tabs to spaces
      for(size_t i = 0; i < line.length(); i++)
        if(line[i] == '\t')
          line[i] = ' ';

      line = Global::trim(line);

      //Upon any input line at all, stop any analysis and output a newline
      if(currentlyAnalyzing) {
        currentlyAnalyzing = false;
        engine->stopAndWait();
        cout << endl;
      }

      if(line.length() == 0)
        continue;

      if(logAllGTPCommunication)
        logger.write("Controller: " + line);

      //Parse id number of command, if present
      size_t digitPrefixLen = 0;
      while(digitPrefixLen < line.length() && Global::isDigit(line[digitPrefixLen]))
        digitPrefixLen++;
      if(digitPrefixLen > 0) {
        hasId = true;
        try {
          id = Global::parseDigits(line,0,digitPrefixLen);
        }
        catch(const IOError& e) {
          cout << "? GTP id '" << id << "' could not be parsed: " << e.what() << endl;
          continue;
        }
        line = line.substr(digitPrefixLen);
      }

      line = Global::trim(line);
      if(line.length() <= 0) {
        cout << "? empty command" << endl;
        continue;
      }

      pieces = Global::split(line,' ');
      for(size_t i = 0; i<pieces.size(); i++)
        pieces[i] = Global::trim(pieces[i]);
      assert(pieces.size() > 0);

      command = pieces[0];
      pieces.erase(pieces.begin());
    }

    bool responseIsError = false;
    bool suppressResponse = false;
    bool shouldQuitAfterResponse = false;
    bool maybeStartPondering = false;
    string response;

    if(command == "protocol_version") {
      response = "2";
    }

    else if(command == "name") {
      response = "KataGo";
    }

    else if(command == "version") {
      if(overrideVersion.size() > 0)
        response = overrideVersion;
      else
        response = Version::getKataGoVersion();
    }

    else if(command == "known_command") {
      if(pieces.size() != 1) {
        responseIsError = true;
        response = "Expected single argument for known_command but got '" + Global::concat(pieces," ") + "'";
      }
      else {
        if(std::find(knownCommands.begin(), knownCommands.end(), pieces[0]) != knownCommands.end())
          response = "true";
        else
          response = "false";
      }
    }

    else if(command == "list_commands") {
      for(size_t i = 0; i<knownCommands.size(); i++) {
        response += knownCommands[i];
        if(i < knownCommands.size()-1)
          response += "\n";
      }
    }

    else if(command == "quit") {
      shouldQuitAfterResponse = true;
      logger.write("Quit requested by controller");
    }

    else if(command == "boardsize" || command == "rectangular_boardsize") {
      int newXSize = 0;
      int newYSize = 0;
      bool suc = false;

      if(pieces.size() == 1) {
        if(contains(pieces[0],':')) {
          vector<string> subpieces = Global::split(pieces[0],':');
          if(subpieces.size() == 2 && Global::tryStringToInt(subpieces[0], newXSize) && Global::tryStringToInt(subpieces[1], newYSize))
            suc = true;
        }
        else {
          if(Global::tryStringToInt(pieces[0], newXSize)) {
            suc = true;
            newYSize = newXSize;
          }
        }
      }
      else if(pieces.size() == 2) {
        if(Global::tryStringToInt(pieces[0], newXSize) && Global::tryStringToInt(pieces[1], newYSize))
          suc = true;
      }

      if(!suc) {
        responseIsError = true;
        response = "Expected int argument for boardsize or pair of ints but got '" + Global::concat(pieces," ") + "'";
      }
      else if(newXSize < 2 || newYSize < 2) {
        responseIsError = true;
        response = "unacceptable size";
      }
      else if(newXSize > Board::MAX_LEN || newYSize > Board::MAX_LEN) {
        responseIsError = true;
        response = Global::strprintf("unacceptable size (Board::MAX_LEN is %d, consider increasing and recompiling)",(int)Board::MAX_LEN);
      }
      else {
        engine->setOrResetBoardSize(cfg,logger,seedRand,newXSize,newYSize);
      }
    }

    else if(command == "clear_board") {
      engine->clearBoard();
      //gomoku
      engine->clearCache();
    }

    else if(command == "komi") {
      float newKomi = 0;
      if(pieces.size() != 1 || !Global::tryStringToFloat(pieces[0],newKomi)) {
        responseIsError = true;
        response = "Expected single float argument for komi but got '" + Global::concat(pieces," ") + "'";
      }
      //GTP spec says that we should accept any komi, but we're going to ignore that.
      else if(isnan(newKomi) || newKomi < Rules::MIN_USER_KOMI || newKomi > Rules::MAX_USER_KOMI) {
        responseIsError = true;
        response = "unacceptable komi";
      }
      else if(!Rules::komiIsIntOrHalfInt(newKomi)) {
        responseIsError = true;
        response = "komi must be an integer or half-integer";
      }
      else {
        engine->updateKomiIfNew(newKomi);
        //In case the controller tells us komi every move, restart pondering afterward.
        maybeStartPondering = engine->bot->getRootHist().moveHistory.size() > 0;
      }
    }

    else if(command == "kata-get-rules") {
      if(pieces.size() != 0) {
        response = "Expected no arguments for kata-get-rules but got '" + Global::concat(pieces," ") + "'";
      }
      else {
        response = engine->getCurrentRules().toJsonStringNoKomi();
      }
    }

    else if(command == "kata-set-rules") {
      string rest = Global::concat(pieces," ");
      bool parseSuccess = false;
      Rules newRules;
      try {
        newRules = Rules::parseRulesWithoutKomi(rest,engine->getCurrentRules().komi);
        parseSuccess = true;
      }
      catch(const StringError& err) {
        responseIsError = true;
        response = "Unknown rules '" + rest + "', " + err.what();
      }
      if(parseSuccess) {
        string error;
        bool suc = engine->setRulesNotIncludingKomi(newRules,error);
        if(!suc) {
          responseIsError = true;
          response = error;
        }
      }
    }

    else if(command == "kata-set-rule") {
      if(pieces.size() != 2) {
        responseIsError = true;
        response = "Expected two arguments for kata-set-rule but got '" + Global::concat(pieces," ") + "'";
      }
      else {
        bool parseSuccess = false;
        Rules currentRules = engine->getCurrentRules();
        Rules newRules;
        try {
          newRules = Rules::updateRules(pieces[0], pieces[1], currentRules);
          parseSuccess = true;
        }
        catch(const StringError& err) {
          responseIsError = true;
          response = err.what();
        }
        if(parseSuccess) {
          string error;
          bool suc = engine->setRulesNotIncludingKomi(newRules,error);
          if(!suc) {
            responseIsError = true;
            response = error;
          }
        }
      }
    }

    else if(command == "time_settings") {
      double mainTime;
      double byoYomiTime;
      int byoYomiStones;
      if(pieces.size() != 3
         || !Global::tryStringToDouble(pieces[0],mainTime)
         || !Global::tryStringToDouble(pieces[1],byoYomiTime)
         || !Global::tryStringToInt(pieces[2],byoYomiStones)
         ) {
        responseIsError = true;
        response = "Expected 2 floats and an int for time_settings but got '" + Global::concat(pieces," ") + "'";
      }
      else if(isnan(mainTime) || mainTime < 0.0 || mainTime > 1e50) {
        responseIsError = true;
        response = "invalid main_time";
      }
      else if(isnan(byoYomiTime) || byoYomiTime < 0.0 || byoYomiTime > 1e50) {
        responseIsError = true;
        response = "invalid byo_yomi_time";
      }
      else if(byoYomiStones < 0 || byoYomiStones > 100000) {
        responseIsError = true;
        response = "invalid byo_yomi_stones";
      }
      else {
        TimeControls tc;
        //This means no time limits, according to gtp spec
        if(byoYomiStones == 0 && byoYomiTime > 0.0) {
          //do nothing, tc already no limits by default
        }
        //Absolute time
        else if(byoYomiStones == 0) {
          tc.originalMainTime = mainTime;
          tc.increment = 0.0;
          tc.originalNumPeriods = 0;
          tc.numStonesPerPeriod = 0;
          tc.perPeriodTime = 0.0;
          tc.mainTimeLeft = mainTime;
          tc.inOvertime = false;
          tc.numPeriodsLeftIncludingCurrent = 0;
          tc.numStonesLeftInPeriod = 0;
          tc.timeLeftInPeriod = 0;
        }
        else {
          tc.originalMainTime = mainTime;
          tc.increment = 0.0;
          tc.originalNumPeriods = 1;
          tc.numStonesPerPeriod = byoYomiStones;
          tc.perPeriodTime = byoYomiTime;
          tc.mainTimeLeft = mainTime;
          tc.inOvertime = false;
          tc.numPeriodsLeftIncludingCurrent = 1;
          tc.numStonesLeftInPeriod = 0;
          tc.timeLeftInPeriod = 0;
        }

        engine->bTimeControls = tc;
        engine->wTimeControls = tc;
      }
    }

    else if(command == "time_left") {
      Player pla;
      double time;
      int stones;
      if(pieces.size() != 3
         || !PlayerIO::tryParsePlayer(pieces[0],pla)
         || !Global::tryStringToDouble(pieces[1],time)
         || !Global::tryStringToInt(pieces[2],stones)
         ) {
        responseIsError = true;
        response = "Expected player and float time and int stones for time_left but got '" + Global::concat(pieces," ") + "'";
      }
      //Be slightly tolerant of negative time left
      else if(isnan(time) || time < -10.0 || time > 1e50) {
        responseIsError = true;
        response = "invalid time";
      }
      else if(stones < 0 || stones > 100000) {
        responseIsError = true;
        response = "invalid stones";
      }
      else {
        TimeControls tc = pla == P_BLACK ? engine->bTimeControls : engine->wTimeControls;
        //Main time
        if(stones == 0) {
          tc.mainTimeLeft = time;
          tc.inOvertime = false;
          tc.numPeriodsLeftIncludingCurrent = tc.originalNumPeriods;
          tc.numStonesLeftInPeriod = 0;
          tc.timeLeftInPeriod = 0;
        }
        else {
          tc.mainTimeLeft = 0.9;
          tc.inOvertime = true;
          tc.numPeriodsLeftIncludingCurrent = 1;
          tc.numStonesLeftInPeriod = stones;
          tc.timeLeftInPeriod = time;
        }
        if(pla == P_BLACK)
          engine->bTimeControls = tc;
        else
          engine->wTimeControls = tc;

        //In case the controller tells us komi every move, restart pondering afterward.
        maybeStartPondering = engine->bot->getRootHist().moveHistory.size() > 0;
      }
    }

    else if(command == "play") {
      Player pla;
      Loc loc;
      if(pieces.size() != 2) {
        responseIsError = true;
        response = "Expected two arguments for play but got '" + Global::concat(pieces," ") + "'";
      }
      else if(!PlayerIO::tryParsePlayer(pieces[0],pla)) {
        responseIsError = true;
        response = "Could not parse color: '" + pieces[0] + "'";
      }
      else if(!tryParseLoc(pieces[1],engine->bot->getRootBoard(),loc)) {
        responseIsError = true;
        response = "Could not parse vertex: '" + pieces[1] + "'";
      }
      else {
        bool suc = engine->play(loc,pla);
        if(!suc) {
          responseIsError = true;
          response = "illegal move";
        }
        maybeStartPondering = true;
      }
    }

    else if(command == "undo") {
      bool suc = engine->undo();
      if(!suc) {
        responseIsError = true;
        response = "cannot undo";
      }
    }

    else if(command == "genmove" || command == "genmove_debug" || command == "search_debug") {
      Player pla;
      if(pieces.size() != 1) {
        responseIsError = true;
        response = "Expected one argument for genmove but got '" + Global::concat(pieces," ") + "'";
      }
      else if(!PlayerIO::tryParsePlayer(pieces[0],pla)) {
        responseIsError = true;
        response = "Could not parse color: '" + pieces[0] + "'";
      }
      else {
        bool debug = command == "genmove_debug" || command == "search_debug";
        bool playChosenMove = command != "search_debug";

        engine->genMove(
          pla,
          logger,searchFactorWhenWinningThreshold,searchFactorWhenWinning,
          cleanupBeforePass,ogsChatToStderr,
          allowResignation,resignThreshold,resignConsecTurns,
          logSearchInfo,debug,playChosenMove,
          response,responseIsError,maybeStartPondering,
          GTPEngine::AnalyzeArgs()
        );
      }
    }

    else if(command == "genmove_analyze" || command == "lz-genmove_analyze" || command == "kata-genmove_analyze") {
      Player pla = engine->bot->getRootPla();
      bool parseFailed = false;
      GTPEngine::AnalyzeArgs args = parseAnalyzeCommand(command, pieces, pla, parseFailed);
      if(parseFailed) {
        responseIsError = true;
        response = "Could not parse genmove_analyze arguments or arguments out of range: '" + Global::concat(pieces," ") + "'";
      }
      else {
        bool debug = false;
        bool playChosenMove = true;

        //Make sure the "equals" for GTP is printed out prior to the first analyze line, regardless of thread racing
        if(hasId)
          cout << "=" << Global::intToString(id) << endl;
        else
          cout << "=" << endl;
        engine->genMove(
          pla,
          logger,searchFactorWhenWinningThreshold,searchFactorWhenWinning,
          cleanupBeforePass,ogsChatToStderr,
          allowResignation,resignThreshold,resignConsecTurns,
          logSearchInfo,debug,playChosenMove,
          response,responseIsError,maybeStartPondering,
          args
        );
        //And manually handle the result as well. In case of error, don't report any play.
        suppressResponse = true;
        if(!responseIsError) {
          cout << response << endl;
          cout << endl;
        }
        else {
          cout << endl;
          if(!loggingToStderr)
            cerr << response << endl;
        }
      }
    }

    else if(command == "clear_cache") {
      engine->clearCache();
    }
    else if(command == "showboard") {
      ostringstream sout;
      engine->bot->getRootHist().printBasicInfo(sout, engine->bot->getRootBoard());
      //Filter out all double newlines, since double newline terminates GTP command responses
      string s = sout.str();
      string filtered;
      for(int i = 0; i<s.length(); i++) {
        if(i > 0 && s[i-1] == '\n' && s[i] == '\n')
          continue;
        filtered += s[i];
      }
      response = Global::trim(filtered);
    }

    else if(command == "fixed_handicap") {
      int n;
      if(pieces.size() != 1) {
        responseIsError = true;
        response = "Expected one argument for fixed_handicap but got '" + Global::concat(pieces," ") + "'";
      }
      else if(!Global::tryStringToInt(pieces[0],n)) {
        responseIsError = true;
        response = "Could not parse number of handicap stones: '" + pieces[0] + "'";
      }
      else if(n < 2) {
        responseIsError = true;
        response = "Number of handicap stones less than 2: '" + pieces[0] + "'";
      }
      else if(!engine->bot->getRootBoard().isEmpty()) {
        responseIsError = true;
        response = "Board is not empty";
      }
      else {
        engine->placeFixedHandicap(n,response,responseIsError);
      }
    }

    else if(command == "place_free_handicap") {
      int n;
      if(pieces.size() != 1) {
        responseIsError = true;
        response = "Expected one argument for place_free_handicap but got '" + Global::concat(pieces," ") + "'";
      }
      else if(!Global::tryStringToInt(pieces[0],n)) {
        responseIsError = true;
        response = "Could not parse number of handicap stones: '" + pieces[0] + "'";
      }
      else if(n < 2) {
        responseIsError = true;
        response = "Number of handicap stones less than 2: '" + pieces[0] + "'";
      }
      else if(!engine->bot->getRootBoard().isEmpty()) {
        responseIsError = true;
        response = "Board is not empty";
      }
      else {
        engine->placeFreeHandicap(n,response,responseIsError);
      }
    }

    else if(command == "set_free_handicap") {
      if(!engine->bot->getRootBoard().isEmpty()) {
        responseIsError = true;
        response = "Board is not empty";
      }
      else {
        vector<Loc> locs;
        int xSize = engine->bot->getRootBoard().x_size;
        int ySize = engine->bot->getRootBoard().y_size;
        Board board(xSize,ySize);
        for(int i = 0; i<pieces.size(); i++) {
          Loc loc;
          bool suc = tryParseLoc(pieces[i],board,loc);
          if(!suc || loc == Board::PASS_LOC) {
            responseIsError = true;
            response = "Invalid handicap location: " + pieces[i];
          }
          locs.push_back(loc);
        }
        for(int i = 0; i<locs.size(); i++)
          board.setStone(locs[i],P_BLACK);

        Player pla = P_WHITE;
        BoardHistory hist(board,pla,engine->getCurrentRules(),0);
        vector<Move> newMoveHistory;
        engine->setPositionAndRules(pla,board,hist,board,pla,newMoveHistory);
      }
    }

    else if(command == "final_score") {
      engine->stopAndWait();

      BoardHistory hist = engine->bot->getRootHist();

      //If the game is finished, then we score the game as-is.
      //If it's not finished, then we try to get a bit clever.
      Player winner = C_EMPTY;
      double finalWhiteMinusBlackScore = 0.0;
      if(hist.isGameFinished) {
        //For GTP purposes, we treat noResult as a draw since there is no provision for anything else.
        winner = hist.winner;
        finalWhiteMinusBlackScore = hist.finalWhiteMinusBlackScore;
      }
      else {
        double lead = engine->computeLead(logger);
        finalWhiteMinusBlackScore = lead;
        winner = lead > 0 ? P_WHITE : lead < 0 ? P_BLACK : C_EMPTY;
      }

      if(winner == C_EMPTY)
        response = "0";
      else if(winner == C_BLACK)
        response = "B+" + Global::strprintf("%.1f",-finalWhiteMinusBlackScore);
      else if(winner == C_WHITE)
        response = "W+" + Global::strprintf("%.1f",finalWhiteMinusBlackScore);
      else
        ASSERT_UNREACHABLE;
    }

    else if(command == "final_status_list") {
      int statusMode = 0;
      if(pieces.size() != 1) {
        responseIsError = true;
        response = "Expected one argument for final_status_list but got '" + Global::concat(pieces," ") + "'";
      }
      else {
        if(pieces[0] == "alive")
          statusMode = 0;
        else if(pieces[0] == "seki")
          statusMode = 1;
        else if(pieces[0] == "dead")
          statusMode = 2;
        else {
          responseIsError = true;
          response = "Argument to final_status_list must be 'alive' or 'seki' or 'dead'";
          statusMode = 3;
        }

        if(statusMode < 3) {
          vector<bool> isAlive = engine->computeAnticipatedStatusesWithOwnership(logger);
          Board board = engine->bot->getRootBoard();
          vector<Loc> locsToReport;

          if(statusMode == 0) {
            for(int y = 0; y<board.y_size; y++) {
              for(int x = 0; x<board.x_size; x++) {
                Loc loc = Location::getLoc(x,y,board.x_size);
                if(board.colors[loc] != C_EMPTY && isAlive[loc])
                  locsToReport.push_back(loc);
              }
            }
          }
          if(statusMode == 2) {
            for(int y = 0; y<board.y_size; y++) {
              for(int x = 0; x<board.x_size; x++) {
                Loc loc = Location::getLoc(x,y,board.x_size);
                if(board.colors[loc] != C_EMPTY && !isAlive[loc])
                  locsToReport.push_back(loc);
              }
            }
          }

          response = "";
          for(int i = 0; i<locsToReport.size(); i++) {
            Loc loc = locsToReport[i];
            if(i > 0)
              response += " ";
            response += Location::toString(loc,board);
          }
        }
      }
    }

    else if(command == "loadsgf") {
      if(pieces.size() != 1 && pieces.size() != 2) {
        responseIsError = true;
        response = "Expected one or two arguments for loadsgf but got '" + Global::concat(pieces," ") + "'";
      }
      else {
        string filename = pieces[0];
        bool parseFailed = false;
        bool moveNumberSpecified = false;
        int moveNumber = 0;
        if(pieces.size() == 2) {
          bool suc = Global::tryStringToInt(pieces[1],moveNumber);
          if(!suc || moveNumber < 0 || moveNumber > 10000000)
            parseFailed = true;
          else {
            moveNumberSpecified = true;
          }
        }
        if(parseFailed) {
          responseIsError = true;
          response = "Invalid value for moveNumber for loadsgf";
        }
        else {
          Board sgfInitialBoard;
          Player sgfInitialNextPla;
          BoardHistory sgfInitialHist;
          Rules sgfRules;
          Board sgfBoard;
          Player sgfNextPla;
          BoardHistory sgfHist;

          bool sgfParseSuccess = false;
          CompactSgf* sgf = NULL;
          try {
            sgf = CompactSgf::loadFile(filename);

            if(!moveNumberSpecified || moveNumber > sgf->moves.size())
              moveNumber = sgf->moves.size();

            sgfRules = sgf->getRulesOrWarn(
              engine->getCurrentRules(), //Use current rules as default
              [&logger](const string& msg) { logger.write(msg); cerr << msg << endl; }
            );
            if(engine->nnEval != NULL) {
              bool rulesWereSupported;
              Rules supportedRules = engine->nnEval->getSupportedRules(sgfRules,rulesWereSupported);
              if(!rulesWereSupported) {
                ostringstream out;
                out << "WARNING: Rules " << sgfRules.toJsonStringNoKomi()
                    << " from sgf not supported by neural net, using " << supportedRules.toJsonStringNoKomi() << " instead";
                logger.write(out.str());
                if(!loggingToStderr)
                  cerr << out.str() << endl;
                sgfRules = supportedRules;
              }
            }

            {
              //See if the rules differ, IGNORING komi differences
              Rules currentRules = engine->getCurrentRules();
              currentRules.komi = sgfRules.komi;
              if(sgfRules != currentRules) {
                ostringstream out;
                out << "Changing rules to " << sgfRules.toJsonStringNoKomi();
                logger.write(out.str());
                if(!loggingToStderr)
                  cerr << out.str() << endl;
              }
            }

            sgf->setupInitialBoardAndHist(sgfRules, sgfInitialBoard, sgfInitialNextPla, sgfInitialHist);
            sgfBoard = sgfInitialBoard;
            sgfNextPla = sgfInitialNextPla;
            sgfHist = sgfInitialHist;
            sgf->playMovesTolerant(sgfBoard,sgfNextPla,sgfHist,moveNumber,preventEncore);

            delete sgf;
            sgf = NULL;
            sgfParseSuccess = true;
          }
          catch(const StringError& err) {
            delete sgf;
            sgf = NULL;
            responseIsError = true;
            response = "Could not load sgf: " + string(err.what());
          }
          catch(...) {
            delete sgf;
            sgf = NULL;
            responseIsError = true;
            response = "Cannot load file";
          }

          if(sgfParseSuccess) {
            if(sgfRules.komi != engine->getCurrentRules().komi) {
              ostringstream out;
              out << "Changing komi to " << sgfRules.komi;
              logger.write(out.str());
              if(!loggingToStderr)
                cerr << out.str() << endl;
            }
            engine->setPositionAndRules(sgfNextPla, sgfBoard, sgfHist, sgfInitialBoard, sgfInitialNextPla, sgfHist.moveHistory);
          }
        }
      }
    }

    else if(command == "analyze" || command == "lz-analyze" || command == "kata-analyze") {
      Player pla = engine->bot->getRootPla();
      bool parseFailed = false;
      GTPEngine::AnalyzeArgs args = parseAnalyzeCommand(command, pieces, pla, parseFailed);

      if(parseFailed) {
        responseIsError = true;
        response = "Could not parse analyze arguments or arguments out of range: '" + Global::concat(pieces," ") + "'";
      }
      else {
        //Make sure the "equals" for GTP is printed out prior to the first analyze line, regardless of thread racing
        if(hasId)
          cout << "=" << Global::intToString(id) << endl;
        else
          cout << "=" << endl;

        engine->analyze(pla, args);

        //No response - currentlyAnalyzing will make sure we get a newline at the appropriate time, when stopped.
        suppressResponse = true;
        currentlyAnalyzing = true;
      }
    }

    else if(command == "stop") {
      //Stop any ongoing ponder or analysis
      engine->stopAndWait();
    }

    else {
      responseIsError = true;
      response = "unknown command";
    }


    //Postprocessing of response
    if(hasId)
      response = Global::intToString(id) + " " + response;
    else
      response = " " + response;

    if(responseIsError)
      response = "?" + response;
    else
      response = "=" + response;

    if(!suppressResponse) {
      cout << response << endl;
      cout << endl;
    }

    if(logAllGTPCommunication)
      logger.write(response);

    if(shouldQuitAfterResponse)
      break;

    if(maybeStartPondering && ponderingEnabled)
      engine->ponder();

  } //Close read loop

  delete engine;
  engine = NULL;
  NeuralNet::globalCleanup();
  ScoreValue::freeTables();

  logger.write("All cleaned up, quitting");
  return 0;
}
