#include "core/global.h"
#include "core/config_parser.h"
#include "core/timer.h"
#include "search/asyncbot.h"
#include "program/setup.h"
#include "program/play.h"
#include "main.h"

#define TCLAP_NAMESTARTSTRING "-" //Use single dashes for all flags
#include <tclap/CmdLine.h>

#include "external/nlohmann_json/json.hpp"

using namespace std;
using json = nlohmann::json;

struct AnalyzeRequest {
  string id;
  int turnNumber;

  Board board;
  BoardHistory hist;
  Player nextPla;

  int64_t maxVisits;
  int analysisPVLen;
  double rootFpuReductionMax;
  double rootPolicyTemperature;
  bool includeOwnership;
};

int MainCmds::analysis(int argc, const char* const* argv) {
  Board::initHash();
  ScoreValue::initTables();
  Rand seedRand;

  string configFile;
  string modelFile;
  int numAnalysisThreads;
  try {
    TCLAP::CmdLine cmd("Run parallel analysis engine", ' ', Version::getKataGoVersionForHelp(),true);
    TCLAP::ValueArg<string> configFileArg("","config","Config file to use (see configs/gtp_example.cfg)",true,string(),"FILE");
    TCLAP::ValueArg<string> modelFileArg("","model","Neural net model file",true,string(),"FILE");
    TCLAP::ValueArg<int> numAnalysisThreadsArg("","analysis-threads","Analysis up to this many positions in parallel",true,0,"THREADS");
    cmd.add(configFileArg);
    cmd.add(modelFileArg);
    cmd.add(numAnalysisThreadsArg);
    cmd.parse(argc,argv);
    configFile = configFileArg.getValue();
    modelFile = modelFileArg.getValue();
    numAnalysisThreads = numAnalysisThreadsArg.getValue();

    if(numAnalysisThreads <= 0 || numAnalysisThreads >= 16384)
      throw new StringError("Invalid value for numAnalysisThreads");
  }
  catch (TCLAP::ArgException &e) {
    cerr << "Error: " << e.error() << " for argument " << e.argId() << endl;
    return 1;
  }

  ConfigParser cfg(configFile);

  Logger logger;
  logger.addFile(cfg.getString("logFile"));
  logger.setLogToStderr(true);

  logger.write("Analysis Engine starting...");
  logger.write(Version::getKataGoVersionForHelp());

  SearchParams params = Setup::loadSingleParams(cfg);
  const int whiteBonusPerHandicapStone = cfg.contains("whiteBonusPerHandicapStone") ? cfg.getInt("whiteBonusPerHandicapStone",0,1) : 0;
  const int analysisPVLen = cfg.contains("analysisPVLen") ? cfg.getInt("analysisPVLen",1,100) : 15;
  const Player perspective = Setup::parseReportAnalysisWinrates(cfg,C_EMPTY);
  const bool assumeMultipleStartingBlackMovesAreHandicap =
    cfg.contains("assumeMultipleStartingBlackMovesAreHandicap") ? cfg.getBool("assumeMultipleStartingBlackMovesAreHandicap") : true;

  NNEvaluator* nnEval;
  {
    Setup::initializeSession(cfg);
    int maxConcurrentEvals = numAnalysisThreads * params.numThreads * 2 + 16; // * 2 + 16 just to give plenty of headroom
    nnEval = Setup::initializeNNEvaluator(
      modelFile,modelFile,cfg,logger,seedRand,maxConcurrentEvals,
      NNPos::MAX_BOARD_LEN,NNPos::MAX_BOARD_LEN
    );
  }
  logger.write("Loaded model "+ modelFile);

  //Check for unused config keys
  cfg.warnUnusedKeys(cerr,&logger);

  ThreadSafeQueue<string*> toWriteQueue;
  auto writeLoop = [&toWriteQueue]() {
    while(true) {
      string* message;
      bool suc = toWriteQueue.waitPop(message);
      if(!suc)
        break;
      cout << *message << endl;
      delete message;
    }
  };

  ThreadSafeQueue<AnalyzeRequest*> toAnalyzeQueue;

  auto analysisLoop = [&params,&nnEval,&logger,perspective,&toAnalyzeQueue,&toWriteQueue]() {
    Rand threadRand;
    string searchRandSeed = Global::uint64ToHexString(threadRand.nextUInt64());
    AsyncBot* bot = new AsyncBot(params, nnEval, &logger, searchRandSeed);

    while(true) {
      AnalyzeRequest* request;
      bool suc = toAnalyzeQueue.waitPop(request);
      if(!suc)
        break;

      SearchParams thisParams = params;
      thisParams.maxVisits = request->maxVisits;
      thisParams.rootFpuReductionMax = request->rootFpuReductionMax;
      thisParams.rootPolicyTemperature = request->rootPolicyTemperature;
      bot->setPosition(request->nextPla,request->board,request->hist);
      bot->setAlwaysIncludeOwnerMap(request->includeOwnership);
      bot->setParams(thisParams);

      Player pla = request->nextPla;
      bot->genMoveSynchronous(pla, TimeControls());

      json ret;
      ret["id"] = request->id;
      ret["turnNumber"] = request->turnNumber;

      int minMoves = 0;
      vector<AnalysisData> buf;
      Search* search = bot->getSearch();
      search->getAnalysisData(buf,minMoves,false,request->analysisPVLen);

      json moveInfos = json::array();
      for(int i = 0; i<buf.size(); i++) {
        const AnalysisData& data = buf[i];
        double winrate = 0.5 * (1.0 + data.winLossValue);
        double utility = data.utility;
        double lcb = Play::getHackedLCBForWinrate(search,data,pla);
        double utilityLcb = data.lcb;
        double scoreMean = data.scoreMean;
        if(perspective == P_BLACK || (perspective != P_BLACK && perspective != P_WHITE && pla == P_BLACK)) {
          winrate = 1.0-winrate;
          lcb = 1.0 - lcb;
          utility = -utility;
          scoreMean = -scoreMean;
          utilityLcb = -utilityLcb;
        }

        json moveInfo;
        moveInfo["move"] = Location::toString(data.move,request->board);
        moveInfo["visits"] = data.numVisits;
        moveInfo["utility"] = utility;
        moveInfo["winrate"] = winrate;
        moveInfo["scoreMean"] = scoreMean;
        moveInfo["scoreStdev"] = data.scoreStdev;
        moveInfo["prior"] = data.policyPrior;
        moveInfo["lcb"] = lcb;
        moveInfo["utilityLcb"] = utilityLcb;
        moveInfo["order"] = data.order;

        json pv = json::array();
        for(int j = 0; j<data.pv.size(); j++)
          pv.push_back(Location::toString(data.pv[j],request->board));
        moveInfo["pv"] = pv;

        moveInfos.push_back(moveInfo);
      }
      ret["moveInfos"] = moveInfos;

      if(request->includeOwnership) {
        static constexpr int ownershipMinVisits = 3;
        vector<double> ownership = search->getAverageTreeOwnership(ownershipMinVisits);

        json ownerships = json::array();
        const Board& board = request->board;
        int nnXLen = bot->getSearch()->nnXLen;
        for(int y = 0; y<board.y_size; y++) {
          for(int x = 0; x<board.x_size; x++) {
            int pos = NNPos::xyToPos(x,y,nnXLen);
            double o;
            if(perspective == P_BLACK || (perspective != P_BLACK && perspective != P_WHITE && pla == P_BLACK))
              o = -ownership[pos];
            else
              o = ownership[pos];
            ownerships.push_back(o);
          }
        }
        ret["ownership"] = ownerships;
      }
      toWriteQueue.forcePush(new string(ret.dump()));

      //Free up bot resources in case it's a while before we do more search
      bot->clearSearch();
      delete request;
    }
    delete bot;
  };

  vector<std::thread> threads;
  threads.push_back(std::thread(writeLoop));
  for(int i = 0; i<numAnalysisThreads; i++) {
    threads.push_back(std::thread(analysisLoop));
  }

  logger.write("Analyzing up to " + Global::intToString(numAnalysisThreads) + " positions at at time in parallel");
  logger.write("Started, ready to begin handling requests");

  auto reportError = [&toWriteQueue](const string& s) {
    json ret;
    ret["error"] = s;
    toWriteQueue.forcePush(new string(ret.dump()));
  };
  auto reportErrorForId = [&toWriteQueue](const string& id, const string& field, const string& s) {
    json ret;
    ret["id"] = id;
    ret["field"] = field;
    ret["error"] = s;
    toWriteQueue.forcePush(new string(ret.dump()));
  };
  auto reportWarningForId = [&toWriteQueue](const string& id, const string& field, const string& s) {
    json ret;
    ret["id"] = id;
    ret["field"] = field;
    ret["warning"] = s;
    toWriteQueue.forcePush(new string(ret.dump()));
  };

  string line;
  json input;
  while(cin) {
    getline(cin,line);
    line = Global::trim(line);
    if(line.length() == 0)
      continue;

    try {
      input = json::parse(line);
    }
    catch(nlohmann::detail::exception& e) {
      reportError(e.what());
      continue;
    }

    if(!input.is_object()) {
      reportError("Request line was valid json but was not an object, ignoring: " + input.dump());
      continue;
    }

    AnalyzeRequest rbase;
    if(input.find("id") == input.end() || !input["id"].is_string()) {
      reportError("Request must have a string \"id\" field");
      continue;
    }
    rbase.id = input["id"].get<string>();

    //Defaults
    rbase.maxVisits = params.maxVisits;
    rbase.analysisPVLen = analysisPVLen;
    rbase.rootFpuReductionMax = params.rootFpuReductionMax;
    rbase.rootPolicyTemperature = params.rootPolicyTemperature;
    rbase.includeOwnership = false;

    auto parseInteger = [&input,&rbase,&reportErrorForId](const char* field, int64_t& buf, int64_t min, int64_t max, const char* errorMessage) {
      try {
        if(!input[field].is_number_integer()) {
          reportErrorForId(rbase.id, field, errorMessage);
          return false;
        }
        int64_t x = input[field].get<int64_t>();
        if(x < min || x > max) {
          reportErrorForId(rbase.id, field, errorMessage);
          return false;
        }
        buf = x;
        return true;
      }
      catch(nlohmann::detail::exception& e) {
        (void)e;
        reportErrorForId(rbase.id, field, errorMessage);
        return false;
      }
    };

    auto parseDouble = [&input,&rbase,&reportErrorForId](const char* field, double& buf, double min, double max, const char* errorMessage) {
      try {
        if(!input[field].is_number()) {
          reportErrorForId(rbase.id, field, errorMessage);
          return false;
        }
        double x = input[field].get<double>();
        if(!isfinite(x) || x < min || x > max) {
          reportErrorForId(rbase.id, field, errorMessage);
          return false;
        }
        buf = x;
        return true;
      }
      catch(nlohmann::detail::exception& e) {
        (void)e;
        reportErrorForId(rbase.id, field, errorMessage);
        return false;
      }
    };

    auto parseBoolean = [&input,&rbase,&reportErrorForId](const char* field, bool& buf, const char* errorMessage) {
      try {
        if(!input[field].is_boolean()) {
          reportErrorForId(rbase.id, field, errorMessage);
          return false;
        }
        buf = input[field].get<bool>();
        return true;
      }
      catch(nlohmann::detail::exception& e) {
        (void)e;
        reportErrorForId(rbase.id, field, errorMessage);
        return false;
      }
    };

    int boardXSize;
    int boardYSize;
    {
      int64_t xBuf;
      int64_t yBuf;
      static const string boardSizeError = string("Must provide an integer from 2 to ") + Global::intToString(Board::MAX_LEN);
      if(input.find("boardXSize") == input.end()) {
        reportErrorForId(rbase.id, "boardXSize", boardSizeError.c_str());
        continue;
      }
      if(input.find("boardYSize") == input.end()) {
        reportErrorForId(rbase.id, "boardYSize", boardSizeError.c_str());
        continue;
      }
      if(!parseInteger("boardXSize", xBuf, 2, Board::MAX_LEN, boardSizeError.c_str())) {
        reportErrorForId(rbase.id, "boardXSize", boardSizeError.c_str());
        continue;
      }
      if(!parseInteger("boardYSize", yBuf, 2, Board::MAX_LEN, boardSizeError.c_str())) {
        reportErrorForId(rbase.id, "boardYSize", boardSizeError.c_str());
        continue;
      }
      boardXSize = (int)xBuf;
      boardYSize = (int)yBuf;
    }

    auto parseBoardLocs = [&input,boardXSize,boardYSize,&rbase,&reportErrorForId](const char* field, vector<Move>& buf, bool allowPass) {
      buf.clear();
      if(!input[field].is_array()) {
        reportErrorForId(rbase.id, field, "Must be an array of pairs of the form: [\"b\" or \"w\", GTP board vertex]");
        return false;
      }
      for(auto& elt : input[field]) {
        if(!elt.is_array() || elt.size() != 2) {
          reportErrorForId(rbase.id, field, "Must be an array of pairs of the form: [\"b\" or \"w\", GTP board vertex]");
          return false;
        }

        string s0;
        string s1;
        try {
          s0 = elt[0].get<string>();
          s1 = elt[1].get<string>();
        }
        catch(nlohmann::detail::exception& e) {
          (void)e;
          reportErrorForId(rbase.id, field, "Must be an array of pairs of the form: [\"b\" or \"w\", GTP board vertex]");
          return false;
        }

        Player pla;
        if(!PlayerIO::tryParsePlayer(s0,pla)) {
          reportErrorForId(rbase.id, field, "Could not parse player: " + s0);
          return false;
        }

        Loc loc;
        if(!Location::tryOfString(s1, boardXSize, boardYSize, loc) ||
           (!allowPass && loc == Board::PASS_LOC) ||
           (loc == Board::NULL_LOC)) {
          reportErrorForId(rbase.id, field, "Could not parse board location: " + s1);
          return false;
        }
        buf.push_back(Move(loc,pla));
      }
      return true;
    };

    vector<Move> placements;
    if(input.find("initialStones") != input.end()) {
      if(!parseBoardLocs("initialStones", placements, false))
        continue;
    }
    vector<Move> moveHistory;
    if(input.find("moves") != input.end()) {
      if(!parseBoardLocs("moves", moveHistory, true))
        continue;
    }
    else {
      reportErrorForId(rbase.id, "moves", "Must specify an array of [player,location] pairs");
      continue;
    }
    Player initialPlayer = C_EMPTY;
    if(input.find("initialPlayer") != input.end()) {
      string s = input["initialPlayer"].get<string>();
      if(!PlayerIO::tryParsePlayer(s,initialPlayer)) {
        reportErrorForId(rbase.id, "initialPlayer", "Must be \"b\" or \"w\"");
        continue;
      }
    }

    vector<bool> shouldAnalyze(moveHistory.size()+1,false);
    if(input.find("analyzeTurns") != input.end()) {
      vector<int> analyzeTurns;
      try {
        analyzeTurns = input["analyzeTurns"].get<vector<int> >();
      }
      catch(nlohmann::detail::exception& e) {
        (void)e;
        reportErrorForId(rbase.id, "analyzeTurns", "Must specify an array of integers indicating turns to analyze");
        continue;
      }

      bool failed = false;
      for(int i = 0; i<analyzeTurns.size(); i++) {
        int turnNumber = analyzeTurns[i];
        if(turnNumber < 0 || turnNumber >= shouldAnalyze.size()) {
          reportErrorForId(rbase.id, "analyzeTurns", "Invalid turn number: " + Global::intToString(turnNumber));
          failed = true;
          break;
        }
        shouldAnalyze[turnNumber] = true;
      }
      if(failed)
        continue;
    }
    else {
      shouldAnalyze[shouldAnalyze.size()-1] = true;
    }

    Rules rules;
    if(input.find("rules") != input.end()) {
      string s = input["rules"].get<string>();
      if(!Rules::tryParseRules(s,rules)) {
        reportErrorForId(rbase.id, "rules", "Unknown rules: " + s);
        continue;
      }
    }
    else {
      reportErrorForId(rbase.id, "rules", "Must specify rules string, such as \"chinese\" or \"tromp-taylor\", or \"koPOSITIONALscoreAREAsui0\".");
      continue;
    }
    if(input.find("komi") != input.end()) {
      double komi;
      bool suc = parseDouble("komi", komi, -100.0, 100.0, "Must be a integer or half-integer from -100.0 to 100.0");
      if(!suc)
        continue;
      rules.komi = (float)komi;
      if(!Rules::komiIsIntOrHalfInt(rules.komi)) {
        reportErrorForId(rbase.id, "rules", "Must be a integer or half-integer from -100.0 to 100.0");
        continue;
      }
    }

    if(input.find("maxVisits") != input.end()) {
      bool suc = parseInteger("maxVisits", rbase.maxVisits, 1, (int64_t)1 << 50, "Must be an integer from 1 to 2^50");
      if(!suc)
        continue;
    }
    if(input.find("analysisPVLen") != input.end()) {
      int64_t buf;
      bool suc = parseInteger("analysisPVLen", buf, 1, 100, "Must be an integer from 1 to 100");
      if(!suc)
        continue;
      rbase.analysisPVLen = (int)buf;
    }
    if(input.find("rootFpuReductionMax") != input.end()) {
      bool suc = parseDouble("rootFpuReductionMax", rbase.rootFpuReductionMax, 0.0, 2.0, "Must be a number from 0.0 to 2.0");
      if(!suc)
        continue;
    }
    if(input.find("rootPolicyTemperature") != input.end()) {
      bool suc = parseDouble("rootPolicyTemperature", rbase.rootPolicyTemperature, 0.01, 100.0, "Must be a number from 0.01 to 100.0");
      if(!suc)
        continue;
    }
    if(input.find("includeOwnership") != input.end()) {
      bool suc = parseBoolean("includeOwnership", rbase.includeOwnership, "Must be a boolean");
      if(!suc)
        continue;
    }

    Board board(boardXSize,boardYSize);
    for(int i = 0; i<placements.size(); i++) {
      board.setStone(placements[i].loc,placements[i].pla);
    }

    if(initialPlayer == C_EMPTY) {
      if(moveHistory.size() > 0)
        initialPlayer = moveHistory[0].pla;
      else {
        int nHandicapStones = Play::numHandicapStones(board,moveHistory,assumeMultipleStartingBlackMovesAreHandicap);
        if(nHandicapStones > 0)
          initialPlayer = P_WHITE;
        else
          initialPlayer = P_BLACK;
      }
    }

    int nHandicapStones = Play::numHandicapStones(board,moveHistory,assumeMultipleStartingBlackMovesAreHandicap);
    rules.komi += (float)(nHandicapStones * whiteBonusPerHandicapStone);

    bool rulesWereSupported;
    Rules supportedRules = nnEval->getSupportedRules(rules,rulesWereSupported);
    if(!rulesWereSupported) {
      ostringstream out;
      out << "Rules " << rules << " not supported by neural net, using " << supportedRules << " instead";
      reportWarningForId(rbase.id, "rules", out.str());
      rules = supportedRules;
    }

    Player nextPla = initialPlayer;
    BoardHistory hist(board,nextPla,rules,0);

    //Build and enqueue requests
    vector<AnalyzeRequest*> newRequests;
    bool foundIllegalMove =  false;
    for(int turnNumber = 0; turnNumber <= moveHistory.size(); turnNumber++) {
      if(shouldAnalyze[turnNumber]) {
        AnalyzeRequest* newRequest = new AnalyzeRequest();
        newRequest->id = rbase.id;
        newRequest->turnNumber = turnNumber;
        newRequest->board = board;
        newRequest->hist = hist;
        newRequest->nextPla = nextPla;
        newRequest->maxVisits = rbase.maxVisits;
        newRequest->analysisPVLen = rbase.analysisPVLen;
        newRequest->rootFpuReductionMax = rbase.rootFpuReductionMax;
        newRequest->rootPolicyTemperature = rbase.rootPolicyTemperature;
        newRequest->includeOwnership = rbase.includeOwnership;
        newRequests.push_back(newRequest);
      }
      if(turnNumber >= moveHistory.size())
        break;

      Player movePla = moveHistory[turnNumber].pla;
      Loc moveLoc = moveHistory[turnNumber].loc;
      if(movePla != nextPla) {
        hist.clear(board,movePla,rules,hist.encorePhase);
      }

      bool multiStoneSuicideLegal = true; //Tolerate suicide in the moves regardless of stated rules
      if(!board.isLegal(moveLoc,movePla,multiStoneSuicideLegal,rules.scoringRule==Rules::SCORING_CAPTURE,rules.komi-0.5)) {
        reportErrorForId(rbase.id, "moves", "Illegal move " + Global::intToString(turnNumber) + ": " + Location::toString(moveLoc,board));
        foundIllegalMove = true;
        break;
      }
      hist.makeBoardMoveAssumeLegal(board,moveLoc,movePla,NULL);
      nextPla = getOpp(movePla);
    }

    if(foundIllegalMove) {
      for(int i = 0; i<newRequests.size(); i++)
        delete newRequests[i];
      newRequests.clear();
      continue;
    }

    for(int i = 0; i<newRequests.size(); i++)
      toAnalyzeQueue.forcePush(newRequests[i]);
    newRequests.clear();
  }

  toWriteQueue.setReadOnly();
  toAnalyzeQueue.setReadOnly();
  for(int i = 0; i<threads.size(); i++)
    threads[i].join();

  logger.write(nnEval->getModelFileName());
  logger.write("NN rows: " + Global::int64ToString(nnEval->numRowsProcessed()));
  logger.write("NN batches: " + Global::int64ToString(nnEval->numBatchesProcessed()));
  logger.write("NN avg batch size: " + Global::doubleToString(nnEval->averageProcessedBatchSize()));
  delete nnEval;
  NeuralNet::globalCleanup();
  ScoreValue::freeTables();
  logger.write("All cleaned up, quitting");
  return 0;
}
