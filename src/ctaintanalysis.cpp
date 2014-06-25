/*
 * CTaintAnalysis.cpp
 *
 *  Created on: 2013-11-10
 *  Author: Xavier N. Noumbissi
 */

#define DEBUG_TYPE "waint"

#include "ctaintanalysis.h"
#include "cinsensitive.h"
#include "checkformatstring.h"

#include <llvm/Support/InstIterator.h>
#include <llvm/Target/Mangler.h>
#include <llvm/DebugInfo.h>

#include <fstream>
#include <sstream>
#include <algorithm>
#include <sstream>

using std::stringstream;


char CTaintAnalysis::ID = 0;

const unsigned CTaintAnalysis::INDENT_LENGTH = 20;

using namespace llvm; 
STATISTIC(NumWarnings, 	   "Number of emited warnings");
STATISTIC(NumTaintedValUse,"Number of found tainted value usage");
STATISTIC(NumFormatString, "Number of found format string vulnerabilities");
STATISTIC(NumIssues, 	   "Number of found issues");
STATISTIC(NumFunctions,	   "Number of functions found in the module");
STATISTIC(NumContextCalls, "Number of context-sensitive analysis calls");

const string CTaintAnalysis::_taintId("[WAINT]");
const string CTaintAnalysis::_taintSourceFile("cfg/sources.cfg");
const string CTaintAnalysis::_taintSinkFile("cfg/sinks.cfg");
const string CTaintAnalysis::_formatStrFile("cfg/formatspec.cfg");

const char CTaintAnalysis::PERCENT('%');

const unsigned CTaintAnalysis::_SOURCE_ARG_RET(0);
const unsigned CTaintAnalysis::_FUNCTION_NOT_SOURCE(10000);
const unsigned CTaintAnalysis::_FUNCTION_NOT_FORMAT(10000);
const unsigned CTaintAnalysis::_INVALID_FORMAT_POS(5000);

map<string, unsigned> CTaintAnalysis::_taintSources;
map<string, unsigned> CTaintAnalysis::_formatSinks;
map<string, unsigned> CTaintAnalysis::_formatStrPos;
typedef map<string, unsigned>::value_type sourceType;

static bool is_number(string &s)
{
    string::size_type i = 0;
    while ( (i<s.length()) && (std::isdigit(s[i])) ) { ++i; }
    return (s.length() == i);
}


void CTaintAnalysis::addTaintInfo(map<string, unsigned> *target,
    				  	  	  	  string &source,
    				  	  	  	  unsigned taintedPos)
{
	map<string, unsigned>::iterator it;
	it = target->find(source);
	if (it != target->end())	it->second = taintedPos;
	else	target->insert( sourceType(source, taintedPos) );
}

void CTaintAnalysis::readTaintSourceConfig()
{
	//Open the file with taint source functions listed
	std::ifstream srcFile(_taintSourceFile.c_str());
	string line;
	string source;
	string arg;
	unsigned pos;

	while (!srcFile.eof()) {
		std::getline(srcFile, line);
		if (!line.empty()) {
			size_t coma = line.find(SOURCE_ARG_DELIM);
			source = line.substr(0, coma);
			arg = line.substr(coma+1);
			pos = (unsigned) atoi(arg.c_str());
			addTaintInfo(&_taintSources, source, pos);
		}
	}

	string r("__isoc99_scanf");
	addTaintInfo(&_taintSources, r, 1);

	r = "__isoc99_sscanf";
	addTaintInfo(&_taintSources, r, 1);

	srcFile.close();
}

void CTaintAnalysis::readTaintSinkConfig()
{
	//Open the file with taint source functions listed
	std::ifstream srcFile(_taintSinkFile.c_str());
	string line;
	string sink;
	string arg;
	int pos = -1;

	while (!srcFile.eof()) {
		std::getline(srcFile, line);
		if (!line.empty()) {
			size_t coma = line.find(SINK_ARG_DELIM);
			sink = line.substr(0, coma);
			arg = line.substr(coma+1);

			if (string::npos != coma) {
				if (!arg.empty() && is_number(arg)){
					pos = atoi(arg.c_str());
					addTaintInfo(&_formatSinks, sink, pos);
					DEBUG_WITH_TYPE("waint-sinks", errs() << "[WAINT] sink format string function '" << sink << "'\n");
				}
			}
			else{
				addTaintInfo(&_formatSinks, sink, 1);
				DEBUG_WITH_TYPE("waint-sinks", errs() << "[WAINT] sink function '" << sink << "'\n");
			}
		}
	}

	srcFile.close();
}

void CTaintAnalysis::readFormatStrConfig()
{
	//Open the file with taint source functions listed
	std::ifstream srcFile(_formatStrFile.c_str());
	string line;
	string sink;
	string arg;
	int pos = -1;

	while (!srcFile.eof()) {
		std::getline(srcFile, line);
		if (!line.empty()) {
			size_t coma = line.find(SINK_ARG_DELIM);
			sink = line.substr(0, coma);
			arg = line.substr(coma+1);

			if (string::npos != coma) {
				if (!arg.empty() && is_number(arg)){
					pos = atoi(arg.c_str());
					addTaintInfo(&_formatStrPos, sink, pos);
					DEBUG_WITH_TYPE("waint-sinks", errs() << "[WAINT] sink function " << sink
														  << " has its format string at parameter '" << pos << "'\n");
				}
			}
			else{
				addTaintInfo(&_formatStrPos, sink, 1);
				DEBUG_WITH_TYPE("waint-sinks", errs() << "[WAINT] sink function " << sink
						  	  	  	  	  	  	  	  << " has its format string at parameter 1'\n");
			}
		}
	}

	srcFile.close();
}

//**************************************************************************************

CTaintAnalysis::CTaintAnalysis() : ModulePass(ID),
									_intraWasRun(false),
									_interRunning(false),
									_ctxInterRunning(false),
									_pointerMain(0),
									_super(0),
									_predInst(0),
									_module(0)
{
	_super = static_cast<InstVisitor<CTaintAnalysis> *>(this);
	readTaintSourceConfig();
	readTaintSinkConfig();
	readFormatStrConfig();
}

//**************************************************************************************

CTaintAnalysis::~CTaintAnalysis()
{
	{//Delete _summaryTable info
		Function *F = 0;
		for(unsigned k = 0; k < _allProcsTPOrder.size(); ++k) {
			F = _allProcsTPOrder[k];
			if (_summaryTable.count(F) > 0)	delete _summaryTable[F];
		}
	}

	{//Delete warning info
		Function *F = 0;
		for(unsigned k = 0; k < _allProcsTPOrder.size(); ++k) {
			F = _allProcsTPOrder[k];
			if (_allWarnings.count(F) > 0) {
				set<AnalysisIssue *> *issues = _allWarnings[F];
				if (issues)  {
					for(set<AnalysisIssue *>::iterator it = issues->begin(), itE = issues->end(); it != itE; ++it)
						delete (*it);
				}
				delete issues;
			}
		}
	}

	{
		map<unsigned, set<AnalysisIssue *> *>::iterator it = _lineToWarning.begin(),
				itE = _lineToWarning.end();
		set<AnalysisIssue *> *issues = 0;
		for(;it != itE; ++it) {
			issues = it->second;
			delete issues;
		}
	}

	/*{
		typedef map<Value *, vector<Instruction *> * >::iterator it_type;
		vector<Instruction *> *insts = 0;
		for(it_type it = _valueToTaintInst.begin(), itEnd = _valueToTaintInst.end();
				it != itEnd; ++it) {
		    insts = it->second;
		    if (insts){
		    	insts->clear();
		    	delete insts;
		    }
		}
	}*/

}

//**************************************************************************************

void CTaintAnalysis::printSummaryTable()
{
	DEBUG_WITH_TYPE("waint-warnings", errs() << "Summary Table Information ++++++++++++++\n");
	for(MFuncSumIterator it = _summaryTable.begin(), itEnd = _summaryTable.end();
			it != itEnd; ++it) {
		Function *f = it->first;
		vector<bool> *taintInfo = it->second;
		if (!taintInfo) continue;

		DEBUG_WITH_TYPE("waint-warnings", errs() << "Function " << f->getName() << "\n");

		unsigned s = taintInfo->size();
		unsigned ri = s-1;
		for(unsigned k = 0; k < s; ++k) {
			if ( !taintInfo->at(k) ) continue;
			if (k == ri)	DEBUG_WITH_TYPE("waint-warnings", errs().indent(INDENT_LENGTH) << "Return value is tainted\n");
			else	DEBUG_WITH_TYPE("waint-warnings", errs().indent(INDENT_LENGTH) << "Parameter " << k+1 << " is tainted\n");
		}
	}
}

//**************************************************************************************

void CTaintAnalysis::printSummaryTableInfo(Function *f)
{
	vector<bool> *taintInfo = _summaryTable[f];
	if (!taintInfo) return;

	unsigned s = taintInfo->size();
	unsigned ri = s-1;
	DEBUG_WITH_TYPE("waint-warnings", errs() << "Function " << f->getName() << "\n");
	for(unsigned k = 0; k < s; ++k) {
		if ( !taintInfo->at(k) ) continue;
		if (k == ri)	DEBUG_WITH_TYPE("waint-warnings", errs().indent(INDENT_LENGTH) << "Return value is tainted\n");
		else	DEBUG_WITH_TYPE("waint-warnings", errs().indent(INDENT_LENGTH) << "Parameter " << k+1 << " is tainted\n"); //We add +1 since function parameters start
		//with 1 in Waint. 0 is reserved for the return value
	}
}

//**************************************************************************************

void CTaintAnalysis::printSummaryTableInfo(Function *f, unsigned param)
{
	vector<bool> *taintInfo = _summaryTable[f];
	if (!taintInfo) return;

	unsigned s = taintInfo->size();
	if (param < s) {
		DEBUG_WITH_TYPE("waint-warnings", errs().indent(INDENT_LENGTH) << "From function " << f->getName());
		if (param == s-1)	DEBUG_WITH_TYPE("waint-warnings", errs().indent(INDENT_LENGTH) << ": return value is tainted\n");
		else	DEBUG_WITH_TYPE("waint-warnings", errs().indent(INDENT_LENGTH) << ": parameter " << param << " is tainted\n");
	}

}

//**************************************************************************************

void CTaintAnalysis::printTaintHistoryList(set<AnalysisIssue *> values)
{
	Instruction *curI = 0;
	Function *f = 0;
	AnalysisIssue *issue = 0;
	vector<Instruction *> *taintingHist;

	for (set<AnalysisIssue *>::iterator it = values.begin(), itE = values.end(); it != itE; ++it) {
		issue = (*it);

		issue->print();

		taintingHist = _valueToTaintInst[issue->getValue()];

		if (taintingHist) {
			errs().indent(INDENT_LENGTH) << "Taint History (size: " << taintingHist->size() << ")\n";

			for (vector<Instruction *>::iterator it = taintingHist->begin(), itE = taintingHist->end();
					it != itE; ++it) {
				curI = (*it);

				errs().indent(INDENT_LENGTH); curI->print(errs()); errs() << " line [" << getLineNumber(*curI) << "] \n";

				f = issue->getFunction();
				if (f) {
					if (_summaryTable.count(f) > 0) printSummaryTableInfo(f, issue->getFuncParam());
				}
			}
		}

	}
}

//**************************************************************************************

void CTaintAnalysis::getAnalysisUsage(AnalysisUsage &AU) const
{
	AU.setPreservesAll();
	AU.addRequired<CallGraph> ();
	AU.addRequired<EQTDDataStructures> ();
}

//**************************************************************************************

bool CTaintAnalysis::runOnModule(Module &m)
{
	errs() << "module identifier is " << m.getModuleIdentifier() << "\n";

	_module = &m;
	_aliasInfo = &getAnalysis<EQTDDataStructures>();
	_cg = &getAnalysis<CallGraph>();

	//getOption("TIMING", TIMING);
	//if(TIMING) assert(0); // make sure assertions are disabled
	//if(!TIMING) Statistics();

	assert(_aliasInfo && "An instance of DSA could not be created!");
	assert(_cg && "An instance of the callgraph could not be created!");

	_globalsGraph = _aliasInfo->getGlobalsGraph();

	Function *f = 0;
	for(CallGraph::iterator pc = _cg->begin(), Epc = _cg->end();
			pc != Epc;
			++pc) {
		f = const_cast<Function *>(pc->first);

		//We only handle function defined in the code
		if (!f)	continue;

		if(f->isDeclaration())	continue;

		++NumFunctions;
		string fName(f->getName().str());
		DEBUG_WITH_TYPE("waint-functions", errs() << "discovered function '" << fName << "'\n");

		//TODO: use function signature as key instead
		_signatureToFunc[fName] = f;
		_allProcsTPOrder.push_back(f);

		{//Initialize _summaryTable info.
			vector<bool> *argVec = new vector<bool>;
			for(Function::arg_iterator A = f->arg_begin(), E=f->arg_end(); A != E; ++A)
				argVec->push_back(false);

			argVec->push_back(false);

			_summaryTable[f] = argVec;
			_allWarnings[f] = new set<AnalysisIssue *>;
		}

		{//Initialize alias info from DSA
			if (_aliasInfo->hasDSGraph(*f))
				_functionToDSGraph[f] = _aliasInfo->getDSGraph(*f);
		}

		if ( !_pointerMain && 0 == fName.compare(ENTRY_POINT) )
			_pointerMain = f;
	}

	//We need the list of procedure in reverse topological order

	_allProcsRTPOrder.insert(_allProcsRTPOrder.begin(), _allProcsTPOrder.begin(), _allProcsTPOrder.end());

	std::reverse(_allProcsRTPOrder.begin(), _allProcsRTPOrder.end());

	//_aliasInfo->print(errs(), &m);

	CTaintIntraProcedural intraFlow(this);
	CTaintContextInterProcedural contextInterFlow(&intraFlow);
	InterProcedural interFlow(&intraFlow);

	intraFlow.doAnalysis();
	interFlow.doAnalysis();
	contextInterFlow.doAnalysis();

	CheckFormatStringPass checkFmtStrPass(this);
	checkFmtStrPass.doAnalysis();

	printTaintHistoryList(_displayOutput);

	return false;
}

//**************************************************************************************

/**
 * Checks if the function with name 'funcName' taints any
 * of its parameters.
 */
unsigned CTaintAnalysis::isTaintSource(string &funcName)
{
	map<string, unsigned>::iterator res;
	res = _taintSources.find(funcName);
	if (res != _taintSources.end())	return res->second;
	return _FUNCTION_NOT_SOURCE;
}

//**************************************************************************************

unsigned CTaintAnalysis::isFormatSink(string &funcName)
{
	map<string, unsigned>::iterator res;
	res = _formatStrPos.find(funcName);
	if (res != _formatStrPos.end())	return res->second;
	return _FUNCTION_NOT_FORMAT;
}

//**************************************************************************************

void CTaintAnalysis::set_diff(set<Value *> &A,
                             set<Value *> &B, 
                             set<Value *> &AMinusB)
{
    AMinusB.clear();
    for(set<Value *>::iterator ps = A.begin(), Eps = A.end();
            ps != Eps;
            ++ps) {
        Value *e = *(ps);
        if (0 == B.count(e))
            AMinusB.insert(e);
      }
}

//**************************************************************************************

bool CTaintAnalysis::merge(BasicBlock *curBB, BasicBlock *succBB)
{
	set<Value *> &curBBOut = _IN[&curBB->back()];
	set<Value *> &succBBIn = _OUT[&succBB->front()];

	set<Value *> inDiff;
	set_diff(curBBOut, succBBIn, inDiff);
	
        if (!inDiff.empty())
        	succBBIn.insert(inDiff.begin(), inDiff.end());
	 
	return (inDiff.size() > 0);
}

//**************************************************************************************

inline void CTaintAnalysis::mergeCopyPredOutFlowToInFlow(Instruction &predInst,
														 Instruction &curInst)
{
  _IN[&curInst].insert(_OUT[&predInst].begin(), _OUT[&predInst].end());
  _OUT[&curInst].insert(_IN[&curInst].begin(), _IN[&curInst].end());
}

//**************************************************************************************

/**
 * Given a value v, and a DSGraph dsg (a DSGraph represents the
 * aliasing relationship within a function), this method checks
 * if v has aliases in function F with dsg as DSGraph.
 */
void CTaintAnalysis::getAliases(Value *v,
								DSGraph *dsg,
								vector<Value *> &aliases)
{
	aliases.clear();
	if (dsg && dsg->hasNodeForValue(v)) {
		//If value v is sharing the same DSNode
		//as another value w, then mayAlias(v,w)
		DSNodeHandle &vHandle = dsg->getNodeForValue(v);
		DSGraph::ScalarMapTy &scalarMap = dsg->getScalarMap();

		DSGraph::ScalarMapTy::iterator sIt = scalarMap.begin();
		DSGraph::ScalarMapTy::iterator EsIt = scalarMap.end();

		aliases.push_back(v);

		const Value *vAlias = 0;
		while(sIt != EsIt) {
			DSNodeHandle &cur = (*sIt).second;
			if (cur == vHandle) {
				vAlias = (*sIt).first;
				if (vAlias && v != vAlias)
					aliases.push_back(const_cast<Value *>(vAlias));
			}
			++sIt;
		}
	}
}

//**************************************************************************************

inline void CTaintAnalysis::vectorUniqueInsert(Instruction *I, vector<Instruction *> &v)
{
	vector<Instruction *>::iterator it = find(v.begin(), v.end(), I);
	if (v.end() == it)	v.push_back(I);
}

//**************************************************************************************

inline void CTaintAnalysis::vectorUniqueInsert2(Value *v, vector<Value *> &instV)
{
	vector<Value *>::iterator it = find(instV.begin(), instV.end(), v);
	if (instV.end() == it) {
		instV.push_back(v);
	}
}

//**************************************************************************************

/**
 * Adds value 'v' to the set OUT[I], denoting that instruction
 * I taints value 'v'.
 */
void CTaintAnalysis::insertToOutFlow(Instruction *I, Value *v)
{
	assert(I && "The instruction to which an outflow is to be added must be non null!");
	assert(v && "The value to insert in the outflow of an instruction must be non null!");

	int n = _OUT[I].count(v);
	if (0 == n) {
		Function *f = I->getParent()->getParent();
		DSGraph *dsg = _functionToDSGraph[f];
		insertToOutFlow(I, v, dsg);
	}
}

//**************************************************************************************

/**
 * Adds value 'v' as tainted by instruction 'I'
 */
void CTaintAnalysis::insertToOutFlow(Instruction *I, Value *v, DSGraph *dsg)
{
	_OUT[I].insert(v);
	DEBUG_WITH_TYPE("waint-tainted", v->print(errs()); errs() << " gets tainted\n");

	{
		if ( 0 == _valueToTaintInst.count(v) )
			_valueToTaintInst[v] = new vector<Instruction *>;
		vectorUniqueInsert(I, *_valueToTaintInst[v]);
	}

	/**
	 * Mark all aliases of v as tainted.
	 */
	if (dsg) {
		vector<Value *> vAliases;
		getAliases(v, dsg, vAliases);
		unsigned s = vAliases.size();
		for(unsigned k = 0; k < s; ++k) {
			_OUT[I].insert(vAliases[k]);
			DEBUG_WITH_TYPE("waint-tainted", vAliases[k]->print(errs());errs() << " \t also gets tainted\n");

			if ( 0 == _valueToTaintInst.count(vAliases[k]) )
				_valueToTaintInst[vAliases[k]] = _valueToTaintInst[v];
		}
	}
}

//**************************************************************************************

/**
 * Checks if there is an edge (caller, callee) in the callgraph
 */
bool CTaintAnalysis::calls(Function *caller, Function *callee)
{
	CallGraphNode *cn = (*_cg)[caller];
	if (cn) {
		//There a node for the caller in the callgraph
		Function *curCallee = 0;
		//Iterate over functions called by 'caller'
		for(CallGraphNode::iterator pn = cn->begin(), Epn = cn->end();
				pn != Epn;
				++pn) {
			curCallee = (*pn).second->getFunction();
			if (curCallee == callee) return true;
		}
	}
	return false;
}

//**************************************************************************************

/**
 * Returns 'true' if value 'v' is tainted at the program point before
 * instruction 'I'.
 */
inline bool CTaintAnalysis::isValueTainted(Instruction *I, Value *v)
{
	return ( 0 < _IN[I].count(v) );
}

//**************************************************************************************

bool CTaintAnalysis::isProcArgTaint(Function *F, unsigned argNo)
{
	vector<bool> * argInfo = _summaryTable[F];
	assert ( (argInfo->size() > argNo) && "Invalid function argument number!" );

	return (*argInfo)[argNo];
}

//**************************************************************************************

void CTaintAnalysis::setProcArgTaint(Function *F, unsigned argNo, bool isTainted)
{
	vector<bool> *argInfo = _summaryTable[F];
	assert ( (argInfo->size() > argNo) && "Invalid function argument number!" );

	(*argInfo)[argNo] = isTainted;
}

//**************************************************************************************

unsigned CTaintAnalysis::getLineNumber(Instruction &I)
{
	if (MDNode *N = I.getMetadata("dbg")) {
	    DILocation Loc(N);
	    return Loc.getLineNumber();
	}
	return -1;
}

//**************************************************************************************

const char *CTaintAnalysis::getConstantCString(ConstantExpr *C)
{
	ArrayRef<Constant *> idxList;
	Constant *elemPtr = ConstantExpr::getGetElementPtr(C, idxList);
	if (elemPtr) {
		if (GlobalVariable *d = dyn_cast<GlobalVariable>(elemPtr->getOperand(0))) {
			if (d->hasInitializer()) {
				StringRef res = cast<ConstantDataArray>(d->getInitializer())->getAsCString();
				return res.str().c_str();
			}
		}
	}
	return 0;
}

//**************************************************************************************

const char *CTaintAnalysis::getCStringInitializer(Value *w, DSGraph *dsg)
{
	vector<Value *> aliases;
	getAliases(w, dsg, aliases);

	if (aliases.size() > 1) {
		Value *v = 0;
		const char *fmt = 0;
		for(unsigned i = 0; i < aliases.size(); ++i) {
			v = aliases[i];
			if (ConstantExpr *C = dyn_cast<ConstantExpr>(v)) {
				fmt = getConstantCString(C);
				if (fmt){
				    //errs() << "## OUF-1 found it " << fmt << "\n";
				    return fmt;
				}
			}
		}
	}

	else if (w->getType()->isPointerTy()) {
		//errs() << "## w "; w->dump(); errs() << " is used in instruction:\n";
		//errs() << *I << "\n";

		//This could be a load for instance. Let's look at all its 'def'.
		for (Value::use_iterator u = w->use_begin(), ue = w->use_end(); u != ue; ++u) {
			if (Instruction *I = dyn_cast<Instruction>(*u)) {
				Function *wCaller = I->getParent()->getParent();

				getAliases(w, _functionToDSGraph[wCaller], aliases);
				for(unsigned i = 0; i < aliases.size(); ++i) {
					if (ConstantExpr *C = dyn_cast<ConstantExpr>(aliases[i])) {
						const char *ouf = getConstantCString(C);
						errs() << "## OUF-2 found it " << ouf << "\n";
						return ouf;
					}
				}

				if (CallInst *CI = dyn_cast<CallInst>(w)) {
					Function *callee = CI->getCalledFunction();
					getAliases(w, _functionToDSGraph[callee], aliases);
					for(unsigned i = 0; i < aliases.size(); ++i) {
						if (ConstantExpr *C = dyn_cast<ConstantExpr>(aliases[i])) {
							const char *ouf = getConstantCString(C);
							errs() << "## OUF-3 found it " << ouf << "\n";
							return ouf;
						}
					}
				}

			}
		}
	}

	else {
        getAliases(w, _globalsGraph, aliases);

	if (aliases.size() > 1) {
		Value *v = 0;
		const char *fmt = 0;
		for(unsigned i = 0; i < aliases.size(); ++i) {
			v = aliases[i];
			if (ConstantExpr *C = dyn_cast<ConstantExpr>(v)) {
				fmt = getConstantCString(C);
				if (fmt){
				    errs() << "## OUF-F found it " << fmt << "\n";
				    return fmt;
				}
			}
		}
	}
	}


    return 0;
}

//**************************************************************************************

bool CTaintAnalysis::checkFormatStr(Function &caller,
				    				Value *curArg,
				    				vector<string::size_type> &result, unsigned line)
{
	//const char *fmt = getCStringInitializer(curArg, _globalsGraph);
	//if (line == 1642 || line == 1650 || line == 748)
	  //errs() << "## DEBUGGING\n";
	const char *fmt = getCStringInitializer(curArg, _functionToDSGraph[&caller]);
	if (fmt) {
		string fmtStr(fmt);

		//All index of percent characters within the format string
		string::size_type found = fmtStr.find(PERCENT);
		string::size_type last = found;

		//In a format string, two consecutive percents (%%) have to be treated as a single %
		bool isDoublePercent = true;

		while(found != string::npos) {
			isDoublePercent = ( last+1 == found );
			if (isDoublePercent) result.pop_back();
			result.push_back(found);
			last = found;
			found = fmtStr.find(PERCENT, found+1);
		}
	}

	//if (line == 1642 || line == 1650 || line == 748) {
	  //  if (fmt)
	    //    errs() << "## fmt: " << fmt << "\n";
	    //else
	      //  errs() << "## fmt: NULL\n";
	  //errs() << "## DEBUGGING END\n";
	//}

	return 0 != fmt;
}

//**************************************************************************************

void CTaintAnalysis::handleFormatSink(CallInst &I, Function &callee, unsigned formatPos)
{
	Function *caller = I.getParent()->getParent();

	unsigned realPos = formatPos - 1;
	unsigned argNr = I.getNumArgOperands();
	assert(realPos < argNr && "The format string parameter is invalid");

	vector<string::size_type> formatString;
	Value *curArg = I.getArgOperand(realPos);

	unsigned line = getLineNumber(I);

	if (checkFormatStr(*caller, curArg, formatString, line)) {
		unsigned restArg = argNr - formatPos;
		unsigned s = formatString.size();
		if (s < restArg) {
			//less arguments than required
		}
	}
	else { /* Missing format string specification */
		AnalysisIssue *issue = new AnalysisIssue(caller, &callee, curArg);
		issue->setLine(line);
		if (_FUNCTION_NOT_FORMAT != formatPos)	issue->setFormatStrPos(formatPos);
		issue->setWarnType(FORMAT_STRING_MISSING_VUL);
		bool isOnNewLine = addIssue(issue);

		if (isOnNewLine) {
			++NumIssues;
			//TODO: refine this number
			++NumFormatString;
			++NumWarnings;

			_displayOutput.insert(issue);
		}
	}

	checkTaintedValueUse(I, callee, formatPos);

}

//**************************************************************************************

void CTaintAnalysis::checkTaintedValueUse(CallInst &I, Function &callee, unsigned formatPos /* = _FUNCTION_NOT_FORMAT */)
{
	Function *caller = I.getParent()->getParent();
	StringRef calleeName = callee.getName();

	unsigned targetPos = _FUNCTION_NOT_FORMAT;
	if (_formatSinks.count(calleeName) > 0)	targetPos = _formatSinks.at(calleeName);

	unsigned line = getLineNumber(I);
	bool isFormatSink = (_FUNCTION_NOT_FORMAT != formatPos);

	bool isOutputVar = false;

	Value *curArg = 0;
	for(unsigned k = 0, display = 1; k < I.getNumArgOperands(); ++k, ++display) {
		curArg = I.getArgOperand(k);
		// Check for tainted value usage
		if ( isValueTainted(&I, curArg) ) {

			isOutputVar = (targetPos == display && isFormatSink); //display = k + 1

			//We don't report the sink parameter of a format string function as tainted value usage
			if (isOutputVar)	continue;

			AnalysisIssue *issue = new AnalysisIssue(caller, &callee, curArg);
			issue->setLine(line);
			if (isFormatSink) issue->setFormatStrPos(formatPos);
			issue->setFuncParam(display);

			if (display == formatPos)	issue->setWarnType(FORMAT_STRING_VUL);
			else if (display > formatPos)	issue->setWarnType(FORMAT_TAINTED_VALUE_USE);
			else {
				assert(_formatSinks.count(calleeName) > 0);
				issue->setWarnType(TAINTED_VALUE_USE);
			}

			if (addIssue(issue)) {
				++NumIssues;
				//TODO: refine this number by checking the reported function and tainted value
				++NumTaintedValUse;
				++NumWarnings;
				_displayOutput.insert(issue);
			}
		}
	}
}

//**************************************************************************************

void CTaintAnalysis::visit(Instruction &I)
{
  if (_predInst)
    mergeCopyPredOutFlowToInFlow(*_predInst, I);
  _super->visit(I);
  _predInst = &I;
}

//**************************************************************************************

void CTaintAnalysis::visitLoadInst(LoadInst &I) {
	DEBUG(errs() << "LOAD [p=*q]: "; I.print(errs()); errs()<<"\n");
	Value *q = I.getPointerOperand();
	if ( isValueTainted(&I, q))	insertToOutFlow(&I, &I);
}

//**************************************************************************************

void CTaintAnalysis::visitStoreInst(StoreInst &I)
{
	DEBUG(errs() << "STORE [*p=q]: ";I.print(errs());errs()<<"\n");
	Value *q = I.getValueOperand();
	Value *p = I.getPointerOperand();
	if ( isValueTainted(&I, q))	insertToOutFlow(&I, p);
}

//**************************************************************************************

void CTaintAnalysis::visitCallInst(CallInst & I)
{
	DEBUG(errs() << "CALL [call func]: ";I.print(errs());errs()<<"\n");
	if (!_intraWasRun) {
		//The intraprocedural analysis has not been run yet
		Function *callee = I.getCalledFunction();
		if (!callee)
			return ;

		string calleeName = callee->getName().str();
		unsigned arg_pos = isTaintSource(calleeName);

		if ( _FUNCTION_NOT_SOURCE == arg_pos ) return;

		unsigned paramSize = I.getNumArgOperands();

		if ( arg_pos != _FUNCTION_NOT_SOURCE && arg_pos < paramSize ) {
			Value *taintedArg = 0;

			if (_SOURCE_ARG_RET == arg_pos)
				taintedArg = &I;
			else
				taintedArg = I.getArgOperand(arg_pos);

			insertToOutFlow(&I, taintedArg);
			DEBUG_WITH_TYPE("waint-sources", errs() << "Found a source "
					<< calleeName << " with arg_pos " << arg_pos << "\n");
		}
		else {
			std::ostringstream msg;
			msg << "Invalid argument position (" << arg_pos << ")"
					<< " max parameters is " << paramSize;
			errs() << msg.str() << "\n";
		}
	}
	else if (_ctxInterRunning) {
		//The context-sensitive analysis is running
		static unsigned depth = 0;

		if (depth > 1)
			return;

		Function *callee = I.getCalledFunction();
		if (!callee)
			return ;

		Function *caller = I.getParent()->getParent();

		if (caller && !calls(caller, callee)) {
		  DEBUG(errs() << "## " << caller->getName()
		      		<< " does not call "<< callee->getName() << " in our callgraph \n");
		  return ;
		}
		vector<bool> *calleeFormals = _summaryTable[callee];

		if (!calleeFormals) {
			DEBUG(errs() << "\tWe do not analyze function '" << callee->getName() << "'\n");
		}
		else {
			++NumContextCalls;
			depth += 1;
			DSGraph *callerDSG = _functionToDSGraph[caller];
			Instruction &calleeFirstI = callee->front().front();
			Function::arg_iterator pFormal = callee->arg_begin(), Epf = callee->arg_end();

			{ /* Before context-sensitive call */
				vector<Value *> fmlAliases;
				Value *curArg = 0;

				for(unsigned k = 0; k < I.getNumArgOperands() && pFormal != Epf; ++pFormal, ++k ) {
					Argument *aFormal = pFormal;
					curArg = I.getArgOperand(k);
					if ( !(*calleeFormals)[k] ) {
						if ( isValueTainted(&I, curArg) )	_IN[&calleeFirstI].insert(curArg);

						if (aFormal) {
							getAliases(aFormal, callerDSG, fmlAliases);
							fmlAliases.insert(fmlAliases.begin(), aFormal);
							for (unsigned j = 0; j < fmlAliases.size(); ++j) {
								if ( fmlAliases[j] && isValueTainted(&I, fmlAliases[j]) )
									_IN[&calleeFirstI].insert(fmlAliases[j]);
							}
						}
					}
				}
			}

			//Now analyze the callee
			//We set the predecessor instruction before analysis of the callee
			_predInst = &I;
			_super->visit(callee);

			set<Value *> outDiff;
			Instruction &calleeLastI = callee->back().back();
			set_diff(_OUT[&calleeLastI], _IN[&calleeFirstI], outDiff);

			{ /* After context-sensitive call */
				DSGraph * calleeDSG = _functionToDSGraph[callee];
				vector<Value *> afterFmlAliases;
				Value *curArg = 0;
				for(unsigned k = 0; k < I.getNumArgOperands() && pFormal != Epf; ++pFormal, ++k ) {
					Argument *aFormal = pFormal;
					curArg = I.getArgOperand(k);
					getAliases(aFormal, calleeDSG, afterFmlAliases);
					afterFmlAliases.insert(afterFmlAliases.begin(), aFormal);

					if ( !(*calleeFormals)[k] ) {
						for(unsigned j = 0; j < afterFmlAliases.size(); ++j) {
							if  (outDiff.count(afterFmlAliases[j]) > 0 || outDiff.count(curArg) > 0) {
								insertToOutFlow(&I, curArg);
								DEBUG(errs() << callee->getName() << ": Setting formal arg ";
								curArg->print(errs()); errs()<< "(" << k << ") as tainted \n");

								setProcArgTaint(callee, k, true);
								break;
							}
						}
					}
				}
			}

			depth -= 1;
		}
	}
	else if (_interRunning) {
		//Context-insensitive analysis
		Function *callee = I.getCalledFunction();
		if (!callee) return ;

		string calleeName = callee->getName().str();
		vector<bool> *calleeFormals = _summaryTable[callee];

		//Check if the function has a non-void type
		if (calleeFormals && !callee->getReturnType()->isVoidTy()) {
			//the last element of the vector is the return value taint information
			bool retIsTaint = calleeFormals->back();

			if (retIsTaint) insertToOutFlow(&I, &I);

			for(unsigned j = 0; j < calleeFormals->size(); ++j)
				if (calleeFormals->at(j) && j < I.getNumArgOperands())
					insertToOutFlow(&I, I.getArgOperand(j));
		}
	}
}

//**************************************************************************************

void CTaintAnalysis::visitCallInstSink(CallInst & I)
{
	DEBUG(errs() << "CALL SINK [call func]: ";I.print(errs());errs()<<"\n");
	Function *callee = I.getCalledFunction();
	if (!callee) return ;

	string calleeName = callee->getName().str();

	unsigned formatPos = isFormatSink(calleeName);

	if (_FUNCTION_NOT_FORMAT != formatPos) {
		DEBUG_WITH_TYPE("waint-sinks", errs() << "[WAINT]Handling sink format string function '" << calleeName << "'\n");
		handleFormatSink(I, *callee, formatPos);
	}
	else if (_formatSinks.count(calleeName) > 0) {
		DEBUG_WITH_TYPE("waint-sinks", errs() << "[WAINT]Handling sink function '" << calleeName << "'\n");
		checkTaintedValueUse(I, *callee);
	}
}

//**************************************************************************************

void CTaintAnalysis::visitReturnInst(ReturnInst &I) {

	Function *F = I.getParent()->getParent();
	DEBUG_WITH_TYPE("waint-summary", errs() << "Analyzing return instruction for " << F->getName() << "\n");
	Value *retVal = I.getReturnValue();

	if (!retVal || retVal->getType()->isVoidTy()){
		DEBUG_WITH_TYPE("waint-summary", errs() << "No return value for: " << F->getName() << "\n");
		return;
	}

	DEBUG_WITH_TYPE("waint-summary", errs() << "\t";retVal->print(errs());errs()<< "\n");

	if (isValueTainted(&I, retVal)) {
		DEBUG_WITH_TYPE("waint-summary", errs() << "\tis tainted\n");
		unsigned retPos = _summaryTable[F]->size() - 1;
		setProcArgTaint(F, retPos, true);
	}
}

//**************************************************************************************

void CTaintAnalysis::visitCastInst(CastInst &I)
{
	DEBUG(errs() << "CAST []: ";I.print(errs());errs()<<"\n");
	//TODO: Fix this. the code below hides some correct bugs warnings.
	//It also adds a lot of false positives
	//Value *v = I.getOperand(0);
	//if (isValueTainted(&I, v))	insertToOutFlow(&I, &I);
}

//**************************************************************************************

void CTaintAnalysis::visitBinaryOperator(BinaryOperator &I)
{
	DEBUG(errs() << "BINARYOPERATOR [r = a+b]: ";I.print(errs());errs()<<"\n");
	Value *left = I.getOperand(0);
	Value *right = I.getOperand(1);
	if (isValueTainted(&I, left) || isValueTainted(&I, right))
	    insertToOutFlow(&I, &I);
}

//**************************************************************************************

void CTaintAnalysis::visitVACopyInst(VACopyInst &I) {
	DEBUG(errs() << "VACOPY: ";I.print(errs());errs()<<"\n");
	Value *src = I.getSrc();
	Value *dest = I.getDest();
	if (isValueTainted(&I, src))	insertToOutFlow(&I, dest);
}

//**************************************************************************************

static RegisterPass<CTaintAnalysis>
X("waint", "Waterloo Taint Analysis Module Pass",
  false /* Only looks at CFG */,
  true /* Analysis Pass */);

