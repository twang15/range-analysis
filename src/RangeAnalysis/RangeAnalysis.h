//===-------------------------- RangeAnalysis.cpp -------------------------===//
//===-----Performs the Range analysis of the variables of the function-----===//
//
//					 The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// This file contains a pass that performs range analysis. The objective of
// the range analysis is to map each integer variable in the program to the
// range of possible values that it might assume through out the program
// execution. Ideally this range should be as constrained as possible, so that
// an optimizing compiler could learn more information about each variable.
// However, the range analysis must be conservative, that it, it will only
// constraint the range of a variable if it can prove that it is safe to do so.
// As an example, consider the program:
//
// i = read();
// if (i < 10) {
//   print (i + 1);
// else {
//   print(i - 1);
// }
//
// In this program we know, from the conditional test, that the value of i in
// the true side of the branch is in the range [-INF, 9], and in the false side
// is in the range [10, +INF].
//
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_RANGEANALYSIS_RANGEANALYSIS_H_
#define LLVM_TRANSFORMS_RANGEANALYSIS_RANGEANALYSIS_H_

#include "llvm/Function.h"
#include "llvm/Pass.h"
#include "llvm/Constants.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ConstantRange.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/TimeValue.h"
#include "llvm/Support/Process.h"
#include <deque>
#include <stack>
#include <set>

//TODO: comment the line below to disable the debug of SCCs and optimize the code
// generated.
//#define SCC_DEBUG

//TODO: comment the line below to disable the dot printing of Constraint Graphs
#define PRINT_DEBUG

#define PRINTCOMPONENT(component) errs() << "\n--------------\n"; \
		for (SmallPtrSetIterator<VarNode*> cit = component.begin(), cend = component.end(); cit != cend; ++cit) { \
			const VarNode *var = *cit; \
			const Value *V = var->getValue(); \
			const Argument *A = NULL; \
			const Instruction *I = NULL; \
			const ConstantInt *CI = NULL; \
			if ((A = dyn_cast<Argument>(V))) { \
				errs() << A->getParent()->getName() << "." << A->getName(); \
			} \
			else if ((I = dyn_cast<Instruction>(V))) { \
				errs() << I->getParent()->getParent()->getName() << "." << I->getParent()->getName() << "." << I->getName(); \
			} \
			else if ((CI = dyn_cast<ConstantInt>(V))) { \
				errs() << CI->getValue(); \
			} \
			errs() << "\n"; \
		} \
		errs() << "\n----------\n";

#ifdef SCC_DEBUG
#define ASSERT(cond,msg) if(!cond){ errs() << "ERROR: " << msg << "\n"; }
#else
#define ASSERT(cond,msg)
#endif

#define NUMBER_FIXED_ITERATIONS 20

using namespace llvm;

namespace {

/// In our range analysis pass we have to perform operations on ranges all the
/// time. LLVM has a class to perform operations on ranges: the class
/// Range. However, the class Range doesn't serve very well
/// for our purposes because we need to perform operations with big numbers
/// (MIN_INT, MAX_INT) a lot of times, without allow these numbers wrap around.
/// And the class Range allows that. So, I'm writing this class to
/// perform operations on ranges, considering these big numbers and without
/// allow them wrap around.
/// The interface of this class is very similar to LLVM's ConstantRange class.
/// TODO: probably, a better idea would be perform our range analysis
/// considering the ranges symbolically, letting them wrap around, 
/// as ConstantRange considers, but it would require big changes 
/// in our algorithm.

enum RangeType {Unknown, Regular, Empty};

class Range {
private:
	APInt l;	// The lower bound of the range.
	APInt u;	// The upper bound of the range.
	RangeType type;

public:
	Range();
	Range(APInt lb, APInt ub, RangeType type);
	~Range();
	APInt getLower() const {return l;}
	APInt getUpper() const {return u;}
	void setLower(const APInt& newl) {this->l = newl;}
	void setUpper(const APInt& newu) {this->u = newu;}
	bool isUnknown() const {return type == Unknown;}
	void setUnknown() {type = Unknown;}
	bool isRegular() const {return type == Regular;}
	void setRegular() {type = Regular;}
	bool isEmpty() const {return type == Empty;}
	void setEmpty() {type = Empty;}
	bool isMaxRange() const;
	void print(raw_ostream& OS) const;
	Range add(const Range& other);
	Range sub(const Range& other);
	Range mul(const Range& other);
	Range udiv(const Range& other);
	Range sdiv(const Range& other);
	Range urem(const Range& other);
	Range srem(const Range& other);
	Range shl(const Range& other);
	Range lshr(const Range& other);
	Range ashr(const Range& other);
	Range And(const Range& other);
	Range Or(const Range& other);
	Range Xor(const Range& other);
	Range truncate(unsigned bitwidht) const;
//	Range signExtend(unsigned bitwidht) const;
//	Range zeroExtend(unsigned bitwidht) const;
	Range sextOrTrunc(unsigned bitwidht) const;
	Range zextOrTrunc(unsigned bitwidht) const;
	Range intersectWith(const Range& other) const;
	Range unionWith(const Range& other) const;
	bool operator==(const Range& other) const;
	bool operator!=(const Range& other) const;
};


/// This class represents a program variable.
class VarNode {
private:
	// The program variable which is represented.
	const Value* V;
	// A Range associated to the variable, that is,
	// its interval inferred by the analysis.
	Range interval;
	// Used by the crop meet operator
	char abstractState;

public:
	VarNode(const Value* V);
	~VarNode();
	/// Initializes the value of the node.
	void init(bool outside);
	/// Returns the range of the variable represented by this node.
	Range getRange() const {return interval;}
	/// Returns the variable represented by this node.
	const Value *getValue() const {return V;}
	/// Changes the status of the variable represented by this node.
	void setRange(const Range& newInterval) {
		this->interval = newInterval;
		
		// Check if lower bound is greater than upper bound. If it is,
		// set range to empty
		if (this->interval.getLower().sgt(this->interval.getUpper())) {
			this->interval.setEmpty();
		}
	}
	/// Pretty print.
	void print(raw_ostream& OS) const;
	char getAbstractState(){ return abstractState; }
	// The possible states are '0', '+', '-' and '?'.
	void storeAbstractState();
};

enum IntervalId {
	BasicIntervalId,
	SymbIntervalId
};

/// This class represents a basic interval of values. This class could inherit
/// from LLVM's Range class, since it *is a Range*. However,
/// LLVM's Range class doesn't have a virtual constructor =/.
class BasicInterval {
private:
	Range range;

public:
	BasicInterval(const Range& range);
	BasicInterval(const APInt& l, const APInt& u);
	BasicInterval();
	virtual ~BasicInterval(); // This is a base class.
	// Methods for RTTI
	virtual IntervalId getValueId() const {return BasicIntervalId;}
	static bool classof(BasicInterval const *) {return true;}
	/// Returns the range of this interval.
	const Range& getRange() const {return this->range;}
	/// Sets the range of this interval to another range.
	void setRange(const Range& newRange) {
		this->range = newRange;
		
		// Check if lower bound is greater than upper bound. If it is,
		// set range to empty
		if (this->range.getLower().sgt(this->range.getUpper())) {
			this->range.setEmpty();
		}
	}
	/// Pretty print.
	virtual void print(raw_ostream& OS) const;
};

/// This is an interval that contains a symbolic limit, which is
/// given by the bounds of a program name, e.g.: [-inf, ub(b) + 1].
class SymbInterval : public BasicInterval {
private:
	// The bound. It is a node which limits the interval of this range.
	const Value* bound;
	// The predicate of the operation in which this interval takes part.
	// It is useful to know how we can constrain this interval
	// after we fix the intersects.
	CmpInst::Predicate pred;

public:
	SymbInterval(const Range& range, const Value* bound, CmpInst::Predicate pred);
	~SymbInterval();
	// Methods for RTTI
	virtual IntervalId getValueId() const {return SymbIntervalId;}
	static bool classof(SymbInterval const *) {return true;}
	static bool classof(BasicInterval const *BI) {
		return BI->getValueId() == SymbIntervalId;
	}
	/// Returns the opcode of the operation that create this interval.
	CmpInst::Predicate getOperation() const {return this->pred;}
	/// Returns the node which is the bound of this interval.
	const Value* getBound() const {return this->bound;}
	/// Replace symbolic intervals with hard-wired constants.
	Range fixIntersects(VarNode* bound, VarNode* sink);
	/// Prints the content of the interval.
	void print(raw_ostream& OS) const;
};

enum OperationId {
	UnaryOpId,
	SigmaOpId,
	BinaryOpId,
	PhiOpId,
	ControlDepId
};

/// This class represents a generic operation in our analysis.
class BasicOp {
private:
	// We do not want people creating objects of this class.
	void operator=(const BasicOp&);
	BasicOp(const BasicOp&);
	// The range of the operation. Each operation has a range associated to it.
	// This range is obtained by inspecting the branches in the source program
	// and extracting its condition and intervals.
	BasicInterval* intersect;
	// The target of the operation, that is, the node which
	// will store the result of the operation.
	VarNode* sink;
	// The instruction that originated this op node
	const Instruction *inst;

protected:
	/// We do not want people creating objects of this class,
	/// but we want to inherit from it.
	BasicOp(BasicInterval* intersect, VarNode* sink, const Instruction *inst);

public:
	/// The dtor. Its virtual because this is a base class.
	virtual ~BasicOp();
	// Methods for RTTI
	virtual OperationId getValueId() const = 0;
	static bool classof(BasicOp const *) {return true;}
	/// Given the input of the operation and the operation that will be
	/// performed, evaluates the result of the operation.
	virtual Range eval() const = 0;
	/// Return the instruction that originated this op node
	const Instruction *getInstruction() const {return inst;}
	/// Replace symbolic intervals with hard-wired constants.
	void fixIntersects(VarNode* V);
	/// Returns the range of the operation.
	BasicInterval* getIntersect() const {return intersect;}
	/// Changes the interval of the operation.
	void setIntersect(const Range& newIntersect) {
		this->intersect->setRange(newIntersect);
	}
	/// Returns the target of the operation, that is,
	/// where the result will be stored.
	const VarNode* getSink() const {return sink;}
	/// Returns the target of the operation, that is,
	/// where the result will be stored.
	VarNode* getSink() {return sink;}
	/// Prints the content of the operation.
	virtual void print(raw_ostream& OS) const = 0;
};

/// A constraint like sink = operation(source) \intersec [l, u]
class UnaryOp : public BasicOp {
private:
	// The source node of the operation.
	VarNode* source;
	// The opcode of the operation.
	unsigned int opcode;
	/// Computes the interval of the sink based on the interval of the sources,
	/// the operation and the interval associated to the operation.
	Range eval() const;

public:
	UnaryOp(BasicInterval* intersect,
		VarNode* sink,
		const Instruction* inst,
		VarNode* source,
		unsigned int opcode);
	~UnaryOp();
	// Methods for RTTI
	virtual OperationId getValueId() const {return UnaryOpId;}
	static bool classof(UnaryOp const *) {return true;}
	static bool classof(BasicOp const *BO) {
		return BO->getValueId() == UnaryOpId ||  BO->getValueId() == SigmaOpId;
	}
	/// Return the opcode of the operation.
	unsigned int getOpcode() const {return opcode;}
	/// Returns the source of the operation.
	VarNode *getSource() const {return source;}
	/// Prints the content of the operation. I didn't it an operator overload
	/// because I had problems to access the members of the class outside it.
	void print(raw_ostream& OS) const;
};

// Specific type of UnaryOp used to represent sigma functions
class SigmaOp: public UnaryOp {
private:
	/// Computes the interval of the sink based on the interval of the sources,
	/// the operation and the interval associated to the operation.
	Range eval() const;
	
	bool unresolved;
	
public:
	SigmaOp(BasicInterval* intersect,
		VarNode* sink,
		const Instruction* inst,
		VarNode* source,
		unsigned int opcode);
	~SigmaOp();
	// Methods for RTTI
	virtual OperationId getValueId() const {return SigmaOpId;}
	static bool classof(SigmaOp const *) {return true;}
	static bool classof(UnaryOp const *UO) {
		return UO->getValueId() == SigmaOpId;
	}
	static bool classof(BasicOp const *BO) {
		return BO->getValueId() == SigmaOpId;
	}
	
	bool isUnresolved() const {return unresolved;}
	void markResolved() {unresolved = false;}
	void markUnresolved() {unresolved = true;}
	
	/// Prints the content of the operation. I didn't it an operator overload
	/// because I had problems to access the members of the class outside it.
	void print(raw_ostream& OS) const;
};

// Specific type of BasicOp used in Nuutila
class ControlDep : public BasicOp {
private:
	VarNode *source;
	Range eval() const;
	void print(raw_ostream& OS) const;

public:
	ControlDep(VarNode* sink, VarNode *source);
	~ControlDep();
	// Methods for RTTI
	virtual OperationId getValueId() const {return ControlDepId;}
	static bool classof(ControlDep const *) {return true;}
	static bool classof(BasicOp const *BO) {
		return BO->getValueId() == ControlDepId;
	}
	/// Returns the source of the operation.
	VarNode *getSource() const {return source;}
};

/// A constraint like sink = phi(src1, src2, ..., srcN)
class PhiOp : public BasicOp {
private:
	// Vector of sources
	SmallVector<const VarNode*, 2> sources;
	// The opcode of the operation.
	unsigned int opcode;
	/// Computes the interval of the sink based on the interval of the sources,
	/// the operation and the interval associated to the operation.
	Range eval() const;

public:
	PhiOp(BasicInterval* intersect,
		VarNode* sink,
		const Instruction* inst,
		unsigned int opcode);
	~PhiOp();
	// Add source to the vector of sources
	void addSource(const VarNode* newsrc);
	// Return source identified by index
	const VarNode *getSource(unsigned index) const {return sources[index];}
	// Methods for RTTI
	virtual OperationId getValueId() const {return PhiOpId;}
	static bool classof(PhiOp const *) {
		return true;
	}
	static bool classof(BasicOp const *BO) {
		return BO->getValueId() == PhiOpId;
	}
	/// Prints the content of the operation. I didn't it an operator overload
	/// because I had problems to access the members of the class outside it.
	void print(raw_ostream& OS) const;
};

/// A constraint like sink = source1 operation source2 intersect [l, u].
class BinaryOp : public BasicOp {
private:
	// The first operand.
	VarNode* source1;
	// The second operand.
	VarNode* source2;
	// The opcode of the operation.
	unsigned int opcode;
	/// Computes the interval of the sink based on the interval of the sources,
	/// the operation and the interval associated to the operation.
	Range eval() const;

public:
	BinaryOp(BasicInterval* intersect,
		VarNode* sink,
		const Instruction* inst,
		VarNode* source1,
		VarNode* source2,
		unsigned int opcode);
	~BinaryOp();
	// Methods for RTTI
	virtual OperationId getValueId() const {return BinaryOpId;}
	static bool classof(BinaryOp const *) {
		return true;
	}
	static bool classof(BasicOp const *BO) {
		return BO->getValueId() == BinaryOpId;
	}
	/// Return the opcode of the operation.
	unsigned int getOpcode() const {return opcode;}
	/// Returns the first operand of this operation.
	VarNode *getSource1() const {return source1;}
	/// Returns the second operand of this operation.
	VarNode *getSource2() const {return source2;}
	/// Prints the content of the operation. I didn't it an operator overload
	/// because I had problems to access the members of the class outside it.
	void print(raw_ostream& OS) const;
};

/// This class is used to store the intersections that we get in the branches.
/// I decided to write it because I think it is better to have an objetc 
/// to store these information than create a lot of maps 
/// in the ConstraintGraph class.
class ValueBranchMap {
private:
	const Value* V;
	const BasicBlock* BBTrue;
	const BasicBlock* BBFalse;
	BasicInterval* ItvT;
	BasicInterval* ItvF;

public:
	ValueBranchMap(const Value* V,
		const BasicBlock* BBTrue,
		const BasicBlock* BBFalse,
		BasicInterval* ItvT,
		BasicInterval* ItvF);
	~ValueBranchMap();
	/// Get the "false side" of the branch
	const BasicBlock *getBBFalse() const {
		return BBFalse;
	}
	/// Get the "true side" of the branch
	const BasicBlock *getBBTrue() const {
		return BBTrue;
	}
	/// Get the interval associated to the true side of the branch
	BasicInterval *getItvT() const {
		return ItvT;
	}
	/// Get the interval associated to the false side of the branch
	BasicInterval *getItvF() const {
		return ItvF;
	}
	/// Get the value associated to the branch.
	const Value *getV() const {
		return V;
	}
	/// Change the interval associated to the true side of the branch
	void setItvT(BasicInterval *Itv) {
		this->ItvT = Itv;
	}
	/// Change the interval associated to the false side of the branch
	void setItvF(BasicInterval *Itv) {
		this->ItvF = Itv;
	}
	/// Clear memory allocated
	void clear();
};

class ValueSwitchMap {
private:
	const Value* V;
	SmallVector<std::pair<BasicInterval*, const BasicBlock*>, 4 > BBsuccs;

public:
	ValueSwitchMap(const Value* V,
		SmallVector<std::pair<BasicInterval*, const BasicBlock*>, 4 > &BBsuccs);

	~ValueSwitchMap();
	/// Get the "false side" of the branch
	const BasicBlock *getBB(unsigned idx) const {
		return BBsuccs[idx].second;
	}
	/// Get the interval associated to the true side of the branch
	BasicInterval *getItv(unsigned idx) const {
		return BBsuccs[idx].first;
	}
	// Get how many cases this switch has
	unsigned getNumOfCases() const {
		return BBsuccs.size();
	}
	/// Get the value associated to the branch.
	const Value *getV() const {
		return V;
	}
	/// Change the interval associated to the true side of the branch
	void setItv(unsigned idx, BasicInterval *Itv) {
		this->BBsuccs[idx].first = Itv;
	}
	/// Clear memory allocated
	void clear();
};

class Profile {
	public:
		class TimeValue: public sys::TimeValue {
			public:
				TimeValue():
					sys::TimeValue(0.0) {}
				
				TimeValue(const sys::TimeValue &from):
					sys::TimeValue(0.0)
				{
					seconds(from.seconds());
					nanoseconds(from.nanoseconds());
				}
					
				TimeValue operator-(const TimeValue &op) {
					return static_cast<TimeValue>(static_cast<sys::TimeValue>(*this) - static_cast<sys::TimeValue>(op));
				}
		};
	
		// Map to store accumulated times
		typedef StringMap<TimeValue> AccTimesMap;
	
	private:
		AccTimesMap accumulatedtimes;
	
	public:
		TimeValue timenow() {
			TimeValue garbage, usertime;
			sys::Process::GetTimeUsage(garbage, usertime, garbage);
			
			return usertime;
		}
	
		void updateTime(StringRef key, const TimeValue &time) {
			accumulatedtimes[key] += time;
		}
		
		double getTimeDouble(StringRef key) {
			return accumulatedtimes[key].seconds() + (0.001) * accumulatedtimes[key].milliseconds();
		}
		
		TimeValue getTime(StringRef key) {
			return accumulatedtimes[key];
		}
		
		void printTime(StringRef key) {
			double time = getTimeDouble(key);
			errs() << time << "\t - " << key << " elapsed time\n";
		}
};

// The VarNodes type.
typedef DenseMap<const Value*, VarNode*> VarNodes;

// The Operations type.
typedef SmallPtrSet<BasicOp*, 64> GenOprs;

// A map from variables to the operations where these variables are used.
typedef DenseMap<const Value*, SmallPtrSet<BasicOp*, 8> > UseMap;

// A map from variables to the operations where these 
// variables are present as bounds
typedef DenseMap<const Value*, SmallPtrSet<BasicOp*, 8> > SymbMap;

// A map from varnodes to the operation in which this variable is defined
typedef DenseMap<const Value*, BasicOp*> DefMap;

typedef DenseMap<const Value*, ValueBranchMap> ValuesBranchMap;

typedef DenseMap<const Value*, ValueSwitchMap> ValuesSwitchMap;

/// This class represents our constraint graph. This graph is used to
/// perform all computations in our analysis.
class ConstraintGraph {
protected:
	// The variables of the source program and the nodes which represent them.
	VarNodes* vars;
	// The operations of the source program and the nodes which represent them.
	GenOprs* oprs;

private:
    // Save the last Function analyzed
    const Function *func;
	// A map from variables to the operations that define them
	DefMap* defMap;
	// A map from variables to the operations where these variables are used.
	UseMap* useMap;
	// A map from variables to the operations where these
	// variables are present as bounds
	SymbMap* symbMap;
	// This data structure is used to store intervals, basic blocks and intervals
	// obtained in the branches.
	ValuesBranchMap* valuesBranchMap;
	ValuesSwitchMap* valuesSwitchMap;
	/// Adds a BinaryOp in the graph.
	void addBinaryOp(const Instruction* I);
	/// Adds a PhiOp in the graph.
	void addPhiOp(const PHINode* Phi);
	// Adds a SigmaOp to the graph.
	void addSigmaOp(const PHINode* Sigma);
	/// Takes an instruction and creates an operation.
	void buildOperations(const Instruction* I);
	void buildValueBranchMap(const BranchInst *br);
	void buildValueSwitchMap(const SwitchInst *sw);
	void buildValueMaps(const Function& F);
	// Perform the widening and narrowing operations

protected:
	void update(const UseMap &compUseMap,
		SmallPtrSet<const Value*, 6>& actv, bool (*meet)(BasicOp* op));
	void update(unsigned nIterations, const UseMap &compUseMap,
			SmallPtrSet<const Value*, 6>& actv);

	virtual void preUpdate(const UseMap &compUseMap,
		SmallPtrSet<const Value*, 6>& entryPoints) = 0;
	virtual void posUpdate(const UseMap &compUseMap,
		SmallPtrSet<const Value*, 6>& activeVars,
		const SmallPtrSet<VarNode*, 32> *component) = 0;

public:
	/// I'm doing this because I want to use this analysis in an
	/// inter-procedural pass. So, I have to receive these data structures as
	// parameters.
	ConstraintGraph(VarNodes *varNodes, GenOprs *genOprs, DefMap *defmap, UseMap *usemap,
		ValuesBranchMap *valuesBranchMap, ValuesSwitchMap *valuesSwitchMap);
	virtual ~ConstraintGraph();
	/// Adds a VarNode in the graph.
	VarNode* addVarNode(const Value* V);
	
	GenOprs* getOprs() {return oprs;}
	DefMap* getDefMap() {return defMap;}
	UseMap* getUseMap() {return useMap;}
	/// Adds an UnaryOp to the graph.
	void addUnaryOp(const Instruction* I);
	/// Iterates through all instructions in the function and builds the graph.
	void buildGraph(const Function& F);
	void buildVarNodes();
	void buildSymbolicIntersectMap();
	UseMap buildUseMap(const SmallPtrSet<VarNode*, 32> &component);
	void propagateToNextSCC(const SmallPtrSet<VarNode*, 32> &component);

	/// Finds the intervals of the variables in the graph.
	void findIntervals();
	void generateEntryPoints(SmallPtrSet<VarNode*, 32> &component, SmallPtrSet<const Value*, 6> &entryPoints);
	void fixIntersects(SmallPtrSet<VarNode*, 32> &component);
	void generateActivesVars(SmallPtrSet<VarNode*, 32> &component, SmallPtrSet<const Value*, 6> &activeVars);

	/// Releases the memory used by the graph.
	void clear();
	/// Prints the content of the graph in dot format. For more informations
	/// about the dot format, see: http://www.graphviz.org/pdf/dotguide.pdf
	void print(const Function& F, raw_ostream& OS) const;
	void printToFile(const Function& F, Twine FileName);
	/// Allow easy printing of graphs from the debugger.
	void dump(const Function& F) const {print(F, dbgs()); dbgs() << '\n'; };
	void printResultIntervals();
	void computeStats();
};

class Cousot: public ConstraintGraph {
private:
	void preUpdate(const UseMap &compUseMap, SmallPtrSet<const Value*, 6>& entryPoints);
	void posUpdate(const UseMap &compUseMap,
		SmallPtrSet<const Value*, 6>& activeVars,
		const SmallPtrSet<VarNode*, 32> *component);

public:
	Cousot(VarNodes *varNodes, GenOprs *genOprs, DefMap *defmap, UseMap *usemap,
		ValuesBranchMap *valuesBranchMap, ValuesSwitchMap *valuesSwitchMap): ConstraintGraph(varNodes, genOprs,
		defmap, usemap, valuesBranchMap, valuesSwitchMap) {}
};

class CropDFS: public ConstraintGraph{
private:
	void preUpdate(const UseMap &compUseMap, SmallPtrSet<const Value*, 6>& entryPoints);
	void posUpdate(const UseMap &compUseMap,
		SmallPtrSet<const Value*, 6>& activeVars,
		const SmallPtrSet<VarNode*, 32> *component);
	void storeAbstractStates(const SmallPtrSet<VarNode*, 32> *component);
	void crop(const UseMap &compUseMap, BasicOp *op);

public:
	CropDFS(VarNodes *varNodes, GenOprs *genOprs, DefMap *defmap, UseMap *usemap,
		ValuesBranchMap *valuesBranchMap, ValuesSwitchMap *valuesSwitchMap): ConstraintGraph(varNodes, genOprs,
		defmap, usemap, valuesBranchMap, valuesSwitchMap) {}
};

class Nuutila {
public:
	VarNodes *variables;
	int index;
	DenseMap<Value*, int> dfs;
	DenseMap<Value*, Value*> root;
	SmallPtrSet<Value*, 32> inComponent;
	DenseMap<Value*, SmallPtrSet<VarNode*, 32> > components;
	std::deque<Value*> worklist;
#ifdef SCC_DEBUG
    bool checkWorklist();
    bool checkComponents();
    bool checkTopologicalSort(UseMap *useMap);
    bool hasEdge(SmallPtrSet<VarNode*, 32> *componentFrom, SmallPtrSet<VarNode*, 32> *componentTo, UseMap *useMap);
#endif
public:
	Nuutila(VarNodes *varNodes, UseMap *useMap, SymbMap *symbMap, bool single = false);
	void addControlDependenceEdges(SymbMap *symbMap, UseMap *useMap, VarNodes* vars);
	void delControlDependenceEdges(UseMap *useMap);
	void visit(Value *V, std::stack<Value*> &stack, UseMap *useMap);
	typedef std::deque<Value*>::reverse_iterator iterator;
	iterator begin() {return worklist.rbegin();}
	iterator end() {return worklist.rend();}
};

class Meet{
public:
	static bool widen(BasicOp* op);
	static bool narrow(BasicOp* op);
	static bool crop(BasicOp* op);
	static bool growth(BasicOp* op);
	static bool fixed(BasicOp* op);
};

class RangeAnalysis{
public:
	/** Gets the maximum bit width of the operands in the instructions of the
	 * function. This function is necessary because the class APInt only
	 * supports binary operations on operands that have the same number of
	 * bits; thus, all the APInts that we allocate to process the function will
	 * have the maximum bit size. The complexity of this function is linear on
	 * the number of operands used in the function.
	 */
	static unsigned getMaxBitWidth(const Function& F);
	static void updateMinMax(unsigned maxBitWidth);
};

template <class CGT>
class InterProceduralRA: public ModulePass, RangeAnalysis{
public:
	static char ID; // Pass identification, replacement for typeid
	InterProceduralRA() : ModulePass(ID) { }
	bool runOnModule(Module &M);
	static unsigned getMaxBitWidth(Module &M);
private:
	void MatchParametersAndReturnValues(Function &F, ConstraintGraph &G);
};

template <class CGT>
class IntraProceduralRA: public FunctionPass, RangeAnalysis{
public:
	static char ID; // Pass identification, replacement for typeid
	IntraProceduralRA() : FunctionPass(ID) {}
	void getAnalysisUsage(AnalysisUsage &AU) const;
	bool runOnFunction(Function &F);
}; // end of class RangeAnalysis

class RangeUnitTest: public ModulePass{
	unsigned total;
	unsigned failed;
	void printStats();
public:
	static char ID; // Pass identification, replacement for typeid
	RangeUnitTest() : ModulePass(ID), total(0), failed(0) {}
	bool runOnModule(Module & M);
};

} // end namespace

#endif /* LLVM_TRANSFORMS_RANGEANALYSIS_RANGEANALYSIS_H_ */
