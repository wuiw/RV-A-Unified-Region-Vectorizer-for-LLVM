//===- remTransform.cpp - Loop Remainder Transforms  ----------------===//
//
//                     The Region Vectorizer
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//

#include "rv/transform/remTransform.h"

#include "rv/analysis/reductionAnalysis.h"
#include "rv/vectorizationInfo.h"
#include "rv/transform/loopCloner.h"
#include "rv/utils.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/MemoryDependenceAnalysis.h"

#include "rvConfig.h"
#include "rv/rvDebug.h"
#include "report.h"


#include <map>
#include <set>

using namespace llvm;

namespace {
  const bool HasActiveVL = true;
}

namespace rv {


struct IterValue {
  Value & val;
  int timeOffset;

  IterValue(Value & _val, int _timeOff)
  : val(_val)
  , timeOffset(_timeOff)
  {}
};

static Value&
CreateSMin(Value & A, Value & B, IRBuilder<> & builder) {
  auto * AltB = builder.CreateICmp(CmpInst::Predicate::ICMP_SLE, &A, &B);
  return *builder.CreateSelect(AltB, &A, &B);
}

static Value*
UnwindCasts(Value* val) {
  auto * castInst = dyn_cast<CastInst>(val);

  while (castInst) {
     val = castInst->getOperand(0);
     castInst = dyn_cast<CastInst>(val);
  }

  return val;
}

class
BranchCondition {
  CmpInst & cmp;
  int cmpReductIdx;
  StridePattern & sp;
  VectorShape redShape;

public:

  BranchCondition(llvm::CmpInst & _cmp, int _cmpReductIdx, StridePattern & _sp, int vectorWidth)
  : cmp(_cmp)
  , cmpReductIdx(_cmpReductIdx)
  , sp(_sp)
  , redShape(sp.getShape(vectorWidth))
  {}

  // return a branch condition object if this condition can be transformed
  static BranchCondition *
  analyze(llvm::CmpInst & cmp, int vectorWidth, ReductionAnalysis & reda, Loop & loop) {
    int reductIdx = -1;
    StridePattern * red = nullptr;

    for (size_t i = 0; i < cmp.getNumOperands(); ++i) {
      auto * opVal = cmp.getOperand(i);
      auto * inst = dyn_cast<Instruction>(opVal);

      if (!inst) continue;

      // step through sext and the like
      auto * baseVal = UnwindCasts(inst);
      inst = dyn_cast<Instruction>(baseVal);
      if (!inst) continue;

      // loop invariant operand
      if (!loop.contains(inst->getParent())) continue;

      auto * valRed = reda.getStrideInfo(*inst);
      if (!valRed) {
        Report() << "reda: is not an inductive stride " << *inst << "\n";
        // loop carried operand is not part of a recognized reduction -> abort
        return nullptr;
      }

      if (reductIdx > -1) {
        Report() << "reda: both cmp operands are loop carried " << *inst << "\n";
        return nullptr; // multiple loop carried values enter this cmp -> abort
      }

      reductIdx = i;
      red = valRed;
    }

    if (!red) {
      Report() << "reda: branch condition does not operate on inductive strides " << cmp << "\n";
      return nullptr;
    }

    return new BranchCondition(cmp, reductIdx, *red, vectorWidth);
  }

  /// re-synthesize this condition with builder @builder
  // call embedFunc for all loop carred instructions
  // the test will be true if it applies for all iterations from [phi to phi + iterOffset]
  Value&
  synthesize(bool exitOnTrue, int iterOffset, std::string suffix, IRBuilder<> & builder, std::set<Value*> * valueSet, std::function<IterValue (Instruction&)> embedFunc) {
    auto * origReduct = cast<Instruction>(cmp.getOperand(cmpReductIdx));

    // the returned value evaluates to the mapped requested value at iteration offset @embReduct.timeOffset
    IterValue embReduct = embedFunc(*origReduct);

    auto * clonedCmp = cast<CmpInst>(cmp.clone());

    // remaining offset amount
    int effectiveOffset = iterOffset - embReduct.timeOffset;

  // iteration interval test
    bool nswFlag = sp.reductor->hasNoSignedWrap();
    bool nuwFlag = sp.reductor->hasNoUnsignedWrap();

    // lift the predicate form NE/EQ to LT/GT
    // the exit is taken on (%v == %n) send to exit if %V >= %n (if posStride)
    auto cmpPred = clonedCmp->getPredicate();
    if ((cmpPred == CmpInst::ICMP_EQ) || (cmpPred == CmpInst::ICMP_NE)) {
      bool posStride = redShape.getStride() > 0;

      CmpInst::Predicate adjustedPred;
      if (nswFlag) {
        adjustedPred  = (exitOnTrue ^ posStride) ? CmpInst::ICMP_SGE : CmpInst::ICMP_SLT;
      } else {
        assert(nuwFlag && "can not extrapolate wrapping exit conditions");
        assert(sp.reductor->hasNoUnsignedWrap());
        adjustedPred  = (exitOnTrue ^ posStride) ? CmpInst::ICMP_UGE : CmpInst::ICMP_ULT;
      }

      clonedCmp->setPredicate(adjustedPred);
    }

    // short cut for same iteration tests
    if (effectiveOffset == 0) {
      clonedCmp->setOperand(cmpReductIdx, &embReduct.val);
      builder.Insert(clonedCmp, cmp.getName().str() + suffix);
      return *clonedCmp;
    }

    // adjust iteration value for the tested iteration
    auto & val = embReduct.val;
    auto * adjusted = builder.CreateAdd(&val, ConstantInt::get(val.getType(), redShape.getStride() * effectiveOffset), "", nswFlag, nuwFlag);
    if (isa<Instruction>(adjusted)) if (valueSet) valueSet->insert(adjusted);
    clonedCmp->setOperand(cmpReductIdx, adjusted);

    // insert cmp
    builder.Insert(clonedCmp, cmp.getName().str() + suffix);

    if (valueSet) valueSet->insert(clonedCmp);

    return *clonedCmp;
  }

  // synthesize the number of remaining operations (using the mapping of @embedFunc))
  Value&
  synthesize_remaining(int iterOffset, std::string suffix, IRBuilder<> & builder, std::set<Value*> * valueSet, std::function<IterValue (Instruction&)> embedFunc) {
    auto * origReduct = cast<Instruction>(cmp.getOperand(cmpReductIdx));

    // the returned value evaluates to the mapped requested value at iteration offset @embReduct.timeOffset
    IterValue embReduct = embedFunc(*origReduct);

    // remaining offset amount
    int effectiveOffset = iterOffset - embReduct.timeOffset;

  // iteration interval test
    bool nswFlag = sp.reductor->hasNoSignedWrap();
    bool nuwFlag = sp.reductor->hasNoUnsignedWrap();

    Value * boundOp = cmp.getOperand(1 - cmpReductIdx);
    Value * rawIterOp = &embReduct.val;

    // materialize a value that represents the actual iteration
    Value * iterOp = rawIterOp;
    if (effectiveOffset != 0) {
      iterOp = builder.CreateAdd(rawIterOp, ConstantInt::get(rawIterOp->getType(), redShape.getStride() * effectiveOffset), "adj." + suffix, nswFlag, nuwFlag);
    }

    // convert this to a stride
    // this code implicitely assumes that the iteration variable does not wrap around
    bool posStride = redShape.getStride() > 0;

    if (posStride) {
      // until ++i == n
      // generate: round_down[ (n - i) / stride ]
      auto * deltaVal = builder.CreateSub(boundOp, iterOp);
      assert((redShape.getStride() == 1) && "TODO: implement for other strides as well.");
      return *deltaVal;

    } else {
      // until --i == n??
      // generate: round_down[ (i - n) / stride ]
      auto * deltaVal = builder.CreateSub(iterOp, boundOp);
      assert((redShape.getStride() == 1) && "TODO: implement for other strides as well.");
      return *deltaVal;
    }
  }
};


typedef std::map<PHINode*, PHINode*> PHIMap;
typedef std::map<Instruction*, Instruction*> InstMap;




struct LoopTransformer {
  Function & F;
  DominatorTree & DT;
  PostDominatorTree & PDT;
  LoopInfo & LI;

  Loop & ScalarL;
  Loop & ClonedL;

  // exit condition builder for the vectorized loop
  BranchCondition & exitConditionBuilder;

  ValueToValueMapTy & vecValMap;
  ReductionAnalysis & reda;
  std::set<Value*> & uniOverrides;

  int vectorWidth;
  int tripAlign;

  // - original loop -
  //
  // entry
  //   |
  // ScalarL <>
  //   |
  //  Exit
  //
  // - transformed loop -
  // entry --> ... // entry branch to the loop
  //   |
  // vecGuard // cost model block and VectorL preheader
  //  |   \
  //  |   VectorL <> // vector loop produced by RV
  //  |    |
  //  |   VecToScalar --> Exit  // reduces vector values to scalars
  //  |   /
  //  ScalarGuard //  scalar loop preHeader
  //    |
  //  ScalarL <> --> Exit

// scalar loop context
  // the old preheader
  BasicBlock * entryBlock;

  // the single loop exit
  BasicBlock * loopExit;

// embedding blocks
  // either dispatches to the vectorized or the scalar loop
  BasicBlock * vecGuardBlock;

  // entry point to the scalar loop (scalar loop prehader)W
  BasicBlock * scalarGuardBlock;

  // exit of the vector loop to the scalar guard
  BasicBlock * vecToScalarExit;

  static
  BasicBlock*
  GetUniqueExiting(llvm::Loop & L) {
    auto * exitBlock = L.getExitBlock();
    for (auto & use : exitBlock->uses()) {
      auto * exitingBr = dyn_cast<BranchInst>(use.getUser());
      if (!exitingBr) continue;
      if (!L.contains(exitingBr->getParent())) continue;

      return exitingBr->getParent();
    }
    return nullptr;
  }

  LoopTransformer(Function & _F, DominatorTree & _DT, PostDominatorTree & _PDT, LoopInfo & _LI, ReductionAnalysis & _reda, std::set<Value*> & _uniOverrides, BranchCondition & _exitBuilder, Loop & _ScalarL, Loop & _ClonedL, ValueToValueMapTy & _vecValMap, int _vectorWidth, int _tripAlign)
  : F(_F)
  , DT(_DT)
  , PDT(_PDT)
  , LI(_LI)
  , ScalarL(_ScalarL)
  , ClonedL(_ClonedL)
  , exitConditionBuilder(_exitBuilder)
  , vecValMap(_vecValMap)
  , reda(_reda)
  , uniOverrides(_uniOverrides)
  , vectorWidth(_vectorWidth)
  , tripAlign(_tripAlign)
  , entryBlock(ScalarL.getLoopPreheader())
  , loopExit(ScalarL.getExitBlock())
  , vecGuardBlock(nullptr)
  , scalarGuardBlock(nullptr)
  , vecToScalarExit(nullptr)
  {
    assert(loopExit && "multi exit loops unsupported (yet)");
    assert(ScalarL.getExitingBlock() && "Scalar loop does not have a unique exiting block (unsupported)");

    // create all basic blocks
    setupControl();

    // repair value flow through the blocks
    repairValueFlow();
  }

  // set-up the vector loop CFG
  void
  setupControl() {
    auto & scalarHead = *ScalarL.getHeader();
    auto & context = scalarHead.getContext();
    auto & vecHead = LookUp(vecValMap, scalarHead);

    std::string loopName = ScalarL.getName().str();
    vecGuardBlock = BasicBlock::Create(context, loopName + ".vecg", &F, &scalarHead);
    scalarGuardBlock = BasicBlock::Create(context, loopName + ".scag", &F, &scalarHead);
    vecToScalarExit = BasicBlock::Create(context, loopName + ".vec2scalar", &F, &scalarHead);

    auto * parentLoop = ScalarL.getParentLoop();
    if (parentLoop) {
      parentLoop->addBasicBlockToLoop(vecGuardBlock, LI);
      parentLoop->addBasicBlockToLoop(scalarGuardBlock, LI);
      parentLoop->addBasicBlockToLoop(vecToScalarExit, LI);
    }

  // branch to vecGuard instead to the scalar loop
    auto * entryTerm = entryBlock->getTerminator();
    for (size_t i = 0; i < entryTerm->getNumSuccessors(); ++i) {
      if (&scalarHead == entryTerm->getSuccessor(i)) {
        entryTerm->setSuccessor(i, vecGuardBlock);
        break;
      }
    }

  // dispatch to vector loop header or the scalar guard
    Value * constTrue = ConstantInt::getTrue(context);
    // TODO cost model / pre-conditions
    auto * vecLoopCond = constTrue;

    BranchInst::Create(&vecHead, scalarGuardBlock, vecLoopCond, vecGuardBlock);

  // make the vector loop exit to vecToScalar
    auto * scaExiting = ScalarL.getExitingBlock();
    auto * scalarTerm = scaExiting->getTerminator();
    auto * vecLoopExiting = &LookUp(vecValMap, *scalarTerm->getParent());
    auto * vecTerm = cast<TerminatorInst>(vecValMap[scalarTerm]);

    for (size_t i = 0; i < scalarTerm->getNumSuccessors(); ++i) {
      if (scalarTerm->getSuccessor(i) != loopExit) continue;
      vecTerm->setSuccessor(i, vecToScalarExit);
      break;
    }

  // branch from vecToScalarExit to the scalarGuard
    // TODO add an early exit branch (whenever no iterations remain)
    auto * remainderCond = constTrue;
    BranchInst::Create(scalarGuardBlock, loopExit, remainderCond, vecToScalarExit);

  // make scalarGuard the new preheader of the scalar loop
    BranchInst::Create(&scalarHead, scalarGuardBlock);

  // update DomTree
    bool scaLoopDominatedExit = DT.dominates(scaExiting, loopExit);
    auto * vecGuardDomNode = DT.addNewBlock(vecGuardBlock, entryBlock); // entry >= vecGuardBlock
    DT.changeImmediateDominator(DT.getNode(&vecHead), vecGuardDomNode); // vecGuardBlock >= vecLoopHead
    auto * scaGuardNode = DT.addNewBlock(scalarGuardBlock, vecGuardBlock); // vecGuardBlock >= scaGuarBlock
    DT.changeImmediateDominator(DT.getNode(&scalarHead), scaGuardNode);
    DT.addNewBlock(vecToScalarExit, vecLoopExiting); // vecLoopExiting >= vecToScalarExit

    if (scaLoopDominatedExit) {
      DT.changeImmediateDominator(loopExit, vecGuardBlock);
    }

  // insert AVL updates
    if (HasActiveVL) SupplementAVLUpdate();

  // update postDomTree
    bool loopPostDomsEntry = PDT.dominates(&scalarHead, entryBlock);
    auto * vecLoopExitingPostDom = PDT.getNode(vecLoopExiting);
    PDT.addNewBlock(scalarGuardBlock, &scalarHead); // scalarHead >= scaGuardBlock
    auto * vecToScalarPostDom = PDT.addNewBlock(vecToScalarExit, loopExit); // loopExit >= vecToScalar
    PDT.changeImmediateDominator(vecLoopExitingPostDom, vecToScalarPostDom); // vecToScalar >= vecLoopExiting

    PDT.addNewBlock(vecGuardBlock, loopExit); // loopExit >= vecGuardBlock(?)

    if (loopPostDomsEntry) {
      // (if scaHead >= preHeader) vecGuard >= preHeader
      PDT.changeImmediateDominator(entryBlock, vecGuardBlock);
    }

    // TODO update PDT
  }

  void
  updateScalarLoopStartValues(ValueToValueMapTy & vecLoopPhis) {
    auto * scalHeader = ScalarL.getHeader();

    IRBuilder<> scaGuardBuilder(scalarGuardBlock, scalarGuardBlock->begin());

    for (auto & scalInst : *scalHeader) {
      if (!isa<PHINode>(scalInst)) break;
      auto & scalarPhi = cast<PHINode>(scalInst);

      std::string phiName = scalarPhi.getName().str();

      int preHeaderIdx = scalarPhi.getBasicBlockIndex(entryBlock);
      assert(preHeaderIdx >= 0);
      auto & initialValue = *scalarPhi.getIncomingValue(preHeaderIdx);

    // create a PHI for every loop header phi in the scalar loop
      // when coming from the vectorGuard -> use the old initial values
      // when coming from the vecToScalarExit -> use the reduced scalar values from the vector loop
      auto & vecPhi = LookUp(vecValMap, scalarPhi);
      auto * latchVal = vecPhi.getIncomingValue(1 - preHeaderIdx);
      auto & vectorLiveOut = cast<Instruction>(*latchVal);

      auto &scaGuardPhi = *scaGuardBuilder.CreatePHI(scalarPhi.getType(), 2, phiName + ".scaGuard");
      scaGuardPhi.addIncoming(&vectorLiveOut, vecToScalarExit);
      scaGuardPhi.addIncoming(&initialValue, vecGuardBlock);

    // take scaGuardPhi from the new preHeader of the scalar loop (scalarGuardBlock)
      scalarPhi.setIncomingBlock(preHeaderIdx, scalarGuardBlock);
      scalarPhi.setIncomingValue(preHeaderIdx, &scaGuardPhi);
    }
  }

  static
  Value&
  ReplicateExpression(std::string suffix, Value & val, ValueToValueMapTy & replMap, std::function<Value* (Instruction&, IRBuilder<>&)> leafFunc, IRBuilder<> & builder) {
    auto * inst = dyn_cast<Instruction>(&val);

    // preserve non-insts
    if (!inst) {
      return val;
    }

    // already copied?
    auto itRepl = replMap.find(inst);
    if (itRepl != replMap.end()) {
      return *itRepl->second;
    }

    // do we have a custom leaf mapping for this value?
    auto * replaceWith = leafFunc(*inst, builder);
    if (replaceWith) {
      return *replaceWith;
    }

    // we must stay in this basic block
    assert(!isa<PHINode>(val) && "can not replicate this!");

    // otw, start cloning
    auto & clone = *inst->clone();
    auto cloneName = inst->getName().str() + suffix;

    // TODO we can not have cycles in legal IR (we break on PHIs). Still insert now to not diverge on degenerate IR.
    replMap[&val] = &clone;

    // remap its operands
    for (size_t i = 0; i < inst->getNumOperands(); ++i) {
      auto & clonedOp = ReplicateExpression(suffix, *inst->getOperand(i), replMap, leafFunc, builder);
      clone.setOperand(i, &clonedOp);
    }

    // insert the clone after its dependences
    builder.Insert(&clone, cloneName);

    return clone;
  }

  static
  int
  GetLoopIncomingIndex(Loop & L, const PHINode & phi) {
    for (size_t i = 0; i < phi.getNumIncomingValues(); ++i) {
      auto * inBlock = phi.getIncomingBlock(i);
      if (L.contains(inBlock)) {
        return i;
      }
    }
    return -1;
  }

  // if in SVE mode set the Active Vector Length appropriately
  void
  SupplementAVLUpdate() {
    auto & vecHeader = LookUp(vecValMap, *ScalarL.getHeader());

    // replicate the vector loop exit condition
    IRBuilder<> builder(&vecHeader, vecHeader.getFirstNonPHIOrDbgOrLifetime()->getIterator());

    // crudely replicated from RepairVectorLoopCondition
    // map vector phis to their shapes
    std::map<Value*, PHINode*> headerPhis;
    std::map<Value*, PHINode*> reductors;
    for (auto & Inst : *ScalarL.getHeader()) {
      auto * phi = dyn_cast<PHINode>(&Inst);
      if (!phi) break;
      auto & vecPhi = LookUp(vecValMap, *phi);
      headerPhis[phi] = &vecPhi;

      auto * sp = reda.getStrideInfo(*phi);
      if (sp) {
        reductors[sp->reductor] = &vecPhi;
      }
    }

    // looks suspiciously similar to VectorLoopCondition
    auto & remTrips =
      exitConditionBuilder.synthesize_remaining(1, ".trips", builder, &uniOverrides,
         [&](Instruction & inst) -> IterValue {
           assert (!isa<CallInst>(inst));

           // loop invariant value
           if (!ScalarL.contains(inst.getParent())) return IterValue(inst, 0);

           // did we hit the header phi
           auto itHeaderPhi = headerPhis.find(&inst);

           // determine the shape of the tested iteration variable
           Value * headerPhi = nullptr;
           int offset = 0;
           if (itHeaderPhi != headerPhis.end()) {
             // we are checking the header phi directly
             headerPhi = itHeaderPhi->second;
             offset = 0;
           } else {
             // we checking the reductor result on the header phi (next iteration value)
             assert(reductors.find(&inst) != reductors.end() && "not testing a reductor");
             auto * matchingHeaderPhi = reductors[&inst];
             headerPhi = matchingHeaderPhi;
             offset = 1; // testing the reductor
           }

           return IterValue(*headerPhi, offset);
          }
    );
    uniOverrides.insert(&remTrips);

  // next min(%remTrips, MVL)
    Constant * rawMVLConst = ConstantInt::get(remTrips.getType(), 256, false);
    auto & rawAVL = CreateSMin(remTrips, *rawMVLConst, builder);
    uniOverrides.insert(&rawAVL);
    Value * AVL = &rawAVL;

    if (rawAVL.getType()->getPrimitiveSizeInBits() > 32) {
      AVL = builder.CreateTrunc(&rawAVL, Type::getInt32Ty(builder.getContext()));
    }


    // synthesize_remaining(int iterOffset, std::string suffix, IRBuilder<> & builder, std::set<Value*> * valueSet, std::function<IterValue (Instruction&)> embedFunc) {
    // insert vector-length update
    auto * lvlFunc = Intrinsic::getDeclaration(F.getParent(), Intrinsic::ve_lvl, {});
    builder.CreateCall(lvlFunc, AVL);

    // reset LVL to full width after the loop
    IRBuilder<> vtBuilder(vecToScalarExit, vecToScalarExit->getFirstNonPHI()->getIterator());
    Constant * MVLConst = ConstantInt::get(Type::getInt32Ty(vtBuilder.getContext()), 256, false);
    vtBuilder.CreateCall(lvlFunc, MVLConst);
  }


  // fix the vector loop exit condition
  void
  RepairVectorLoopCondition(ValueToValueMapTy & vecLoopPhis) {
    auto & vecHead = LookUp(vecValMap, *ScalarL.getHeader());

    // map vector phis to their shapes
    std::map<Value*, PHINode*> headerPhis;
    std::map<Value*, PHINode*> reductors;
    for (auto & Inst : *ScalarL.getHeader()) {
      auto * phi = dyn_cast<PHINode>(&Inst);
      if (!phi) break;
      auto & vecPhi = LookUp(vecValMap, *phi);
      headerPhis[phi] = &vecPhi;

      auto * sp = reda.getStrideInfo(*phi);
      if (sp) {
        reductors[sp->reductor] = &vecPhi;
      }
    }

    // replicate the vector loop exit condition
    // TODO unify with vecGuard synthesis
    auto & scaExiting = *GetUniqueExiting(ScalarL);
    auto & vecExiting = LookUp(vecValMap, scaExiting);
    auto & vecExitingBr = cast<BranchInst>(*vecExiting.getTerminator());
    bool exitOnTrue = ClonedL.contains(vecExitingBr.getSuccessor(0));

    // replicate the vector loop exit condition
    IRBuilder<> builder(&vecHead, vecHead.getTerminator()->getIterator());


    // in case of AVL, check there is at least one remaining iteration
    int iterOffset = HasActiveVL ? vectorWidth + 1 : 2 * vectorWidth;

    auto & exitVal =
      exitConditionBuilder.synthesize(exitOnTrue, iterOffset, ".vecExit", builder, &uniOverrides,
         [&](Instruction & inst) -> IterValue {
           assert (!isa<CallInst>(inst));

           // loop invariant value
           if (!ScalarL.contains(inst.getParent())) return IterValue(inst, 0);

           // did we hit the header phi
           auto itHeaderPhi = headerPhis.find(&inst);

           // determine the shape of the tested iteration variable
           Value * headerPhi = nullptr;
           int offset = 0;
           if (itHeaderPhi != headerPhis.end()) {
             // we are checking the header phi directly
             headerPhi = itHeaderPhi->second;
             offset = 0;
           } else {
             // we checking the reductor result on the header phi (next iteration value)
             assert(reductors.find(&inst) != reductors.end() && "not testing a reductor");
             auto * matchingHeaderPhi = reductors[&inst];
             headerPhi = matchingHeaderPhi;
             offset = 1; // testing the reductor
           }

           return IterValue(*headerPhi, offset);
          }
    );

    // use forwarded exit condition
    vecExitingBr.setCondition(&exitVal);

    // annotate the vector loop exit condition as uniform
    uniOverrides.insert(&exitVal);
  }

  // supplement the vector loop guard condition
  // the Vloop is executed if there is at least one full vector of iterations
  void
  SupplementVectorGuard() {
    // map vector phis to their shapes
    std::map<Value*, rv::VectorShape> valShapes;
    std::map<Value*, PHINode*> reductors;
    for (auto & Inst : *ScalarL.getHeader()) {
      auto * phi = dyn_cast<PHINode>(&Inst);
      if (!phi) break;
      auto * pat = reda.getStrideInfo(*phi);
      if (pat) {
        auto & reductor = *pat->reductor;
        auto redShape = pat->getShape(vectorWidth);
        valShapes[phi] = redShape;
        reductors[&reductor] = phi;
      }
    }

    // replicate the scalar loop exit condition
    auto & vecGuardBr = *cast<BranchInst>(vecGuardBlock->getTerminator());

    // replicate the vector loop exit condition
    IRBuilder<> builder(vecGuardBlock, vecGuardBr.getIterator());

    // replicate the vector loop exit condition
    // TODO unify with vecGuard synthesis
    auto & scaExiting = *GetUniqueExiting(ScalarL);
    auto & vecExiting = LookUp(vecValMap, scaExiting);
    auto & vecExitingBr = cast<BranchInst>(*vecExiting.getTerminator());
    bool exitOnTrue = !ClonedL.contains(vecExitingBr.getSuccessor(0));

    // in case of AVL, check there is at least one remaining iteration
    int iterOffset = HasActiveVL ? 1 : vectorWidth;

    // synthesize(int iterOffset, std::string suffix, IRBuilder<> & builder, std::function<Instruction& (Instruction&)> embedFunc) {
    auto & exitVal =
      exitConditionBuilder.synthesize(exitOnTrue, iterOffset, ".vecGuard", builder, nullptr,
         [&](Instruction & inst) -> IterValue {
           assert (!isa<CallInst>(inst));

           IF_DEBUG errs() << inst << "\n";

           // loop invariant value
           if (!ScalarL.contains(inst.getParent())) return IterValue(inst, 0);

           // did we hit the header phi
           auto itHeaderPhi = valShapes.find(&inst);

           // determine the shape of the tested iteration variable
           PHINode * headerPhi = nullptr;
           int offset = 0;
           if (itHeaderPhi != valShapes.end()) {
             // we are checking the header phi directly
             headerPhi = cast<PHINode>(&inst);
           } else {
             // we checking the reductor result on the header phi (next iteration value)
             assert(reductors.find(&inst) != reductors.end() && "not testing a reductor");
             auto * matchingHeaderPhi = reductors[&inst];
             headerPhi = matchingHeaderPhi;
             // we are testing the reductor that evaluates to the next iteration value
             offset = 1;
           }

           // translate to vector loopt
           auto & vecPhi = cast<PHINode>(LookUp(vecValMap, *headerPhi));

           // FIXME don't reconstruct the init value -> map this somewhere before the transformation
           int initIdx = vecPhi.getBasicBlockIndex(vecGuardBlock);
           auto * initVal = vecPhi.getIncomingValue(initIdx);

           return IterValue(*initVal, offset);
          }
      );

    // TODO negate condition as necessary
    // use forwarded exit condition
    vecGuardBr.setCondition(&exitVal);
  }

  // replicate the scalar loop exit condition in vecToScalarExit
  void
  SupplementVectorExit(ValueToValueMapTy & vecLoopPhis) {
    auto & scaExiting = *GetUniqueExiting(ScalarL);
    auto & vecExiting = LookUp(vecValMap, scaExiting);

    auto & exitingBr = cast<BranchInst>(*scaExiting.getTerminator());
    int exitSuccIdx = exitingBr.getSuccessor(0) == loopExit ? 0 : 1;

    auto & vecExitBr = *cast<BranchInst>(vecToScalarExit->getTerminator());
    IRBuilder<> builder(vecToScalarExit, vecToScalarExit->getTerminator()->getIterator());

    ValueToValueMapTy replMap;

    // branch to the remainder loop as necessary
    if (!HasActiveVL && (tripAlign % vectorWidth != 0)) {
      IF_DEBUG { errs() << "remTrans: need a scalar remainder loop.\n"; }
    // replicate the exit condition
      // replace scalar reductors with their vector-loop versions
      auto & exitVal =
        ReplicateExpression(".v2s", *exitingBr.getCondition(), replMap,
           [&](Instruction & inst, IRBuilder<>&) -> Value* {
             // loop invariant value
             if (!ScalarL.contains(inst.getParent())) return &inst;

             // if we hit a reduction/induction value replace it with its vector version
             if (isa<PHINode>(inst) || reda.getStrideInfo(inst) || reda.getReductionInfo(inst)) {
               auto * loopVal = &LookUp(vecValMap, inst);
               auto * lcssaPhi = PHINode::Create(loopVal->getType(), 1, inst.getName().str() + ".lscca", &*vecToScalarExit->begin());
               lcssaPhi->addIncoming(loopVal, &vecExiting);
               return lcssaPhi;
             }

             // Otw, copy that operation
             return nullptr;
            },
        builder
        );

      vecExitBr.setCondition(&exitVal);

      // swap the exits to negate the condition
      if (exitSuccIdx == 0) {
        vecExitBr.setSuccessor(0, loopExit);
        vecExitBr.setSuccessor(1, scalarGuardBlock);
      }

      return;

    } else {
      // the scalar loop is never executed, unconditionally branch to the loop exit
      vecExitBr.setCondition(ConstantInt::getTrue(vecExitBr.getContext()));
      vecExitBr.setSuccessor(0, loopExit);
      vecExitBr.setSuccessor(1, scalarGuardBlock);
    }
  }


  void
  updateExitLiveOuts(ValueToValueMapTy & vecLiveOuts) {
  // reduce all remaining live outs
    IRBuilder<> exitBuilder(loopExit, loopExit->begin());

    for (auto * BB : ScalarL.blocks()) {
      for (auto & Inst : *BB) {
        PHINode * mergePhi = nullptr;
        for (auto & use : Inst.uses()) {
          auto * userInst = cast<Instruction>(use.getUser());
          if (ScalarL.contains(userInst)) continue;

          auto scaLiveOut = &Inst;
          auto vecLiveOut = &LookUp(vecValMap, Inst);

          if (isa<PHINode>(userInst) && userInst->getParent() == loopExit) {
            auto & userPhi = *cast<PHINode>(userInst);
            int exitingIdx = userPhi.getBasicBlockIndex(ScalarL.getExitingBlock());
            assert(exitingIdx >= 0);

            // vector loop is exiting to this block now as well
            userPhi.addIncoming(vecLiveOut, vecToScalarExit);

          } else {

            // Create a new phi node to receive this value
            if (!mergePhi) {
              std::string liveOutName = scaLiveOut->getName().str();
              mergePhi = exitBuilder.CreatePHI(scaLiveOut->getType(), 2, liveOutName + ".merge");
              mergePhi->addIncoming(scaLiveOut, ScalarL.getExitingBlock());
              mergePhi->addIncoming(vecLiveOut, vecToScalarExit);
              IF_DEBUG { errs() << "\tCreated merge phi " << *mergePhi << "\n"; }
            }

            userInst->setOperand(use.getOperandNo(), mergePhi);
            IF_DEBUG { errs() << "\t- fixed user " << *userInst << "\n"; }
          }
        }
      }
    }
  }

  void
  fixVecLoopHeaderPhis() {
    auto & vecHead = LookUp(vecValMap, *ScalarL.getHeader());
    for (auto & Inst : vecHead) {
      auto * phi = dyn_cast<PHINode>(&Inst);
      if (!phi) break;
      int initOpIdx = phi->getBasicBlockIndex(entryBlock);
      phi->setIncomingBlock(initOpIdx, vecGuardBlock);
    }
  }

  void
  repairValueFlow() {
    ValueToValueMapTy vecLoopPhis, vecLiveOuts;

    // start edge now coming from vecGuardBlock (instead of old preheader entrBlock)
    fixVecLoopHeaderPhis();

    // reduce loop live outs and ALL vector loop phis (that existed in the scalar loop)
    // reduceVectorLiveOuts(vecLoopPhis, vecLiveOuts);

    // let the scalar loop start from the remainder vector loop remainder (if the VL was executed)
    updateScalarLoopStartValues(vecLoopPhis);

    // repair vector loop liveouts
    updateExitLiveOuts(vecLiveOuts);

  // supplement the scalar remainder condition (vecToScalarBlock -> loopExit edge)
    // this edge is taken if no iterations remain for the scalar loop
    SupplementVectorExit(vecLoopPhis);

    // repair the vector loop exit condition to check for the next iteration (instead on the phi + strie * vectorWidth -th)
    RepairVectorLoopCondition(vecLoopPhis);

  // supplement the vector loop guard condition (vecGuardBlock -> vector loop edge)
    // the vector loop will execute on at least one full vector
    SupplementVectorGuard();
  }
};

BranchCondition*
RemainderTransform::analyzeExitCondition(llvm::Loop & L, int vectorWidth) {
  auto * loopExiting = L.getExitingBlock();


  // loop exit conditions constraints
  auto * exitingBr = dyn_cast<BranchInst>(loopExiting->getTerminator());
  if (!exitingBr) {
    Report() << "remTrans: exiting terminator is not a branch: " << *loopExiting->getTerminator() << "\n";
    return nullptr;
  }

  auto * exitingCmp = dyn_cast<CmpInst>(exitingBr->getCondition());
  if (!exitingCmp) {
    Report() << "remTrans: loop exit condition is not a compare: " << *exitingBr->getCondition() << "\n";
    return nullptr;
  }

  return BranchCondition::analyze(*exitingCmp, vectorWidth, reda, L);
}

bool
RemainderTransform::canTransformLoop(llvm::Loop & L) {
  auto * loopExiting = L.getExitingBlock();
  if (!loopExiting) {
    Report() << "remTrans: multi-exit loops not supported yet\n";
    return false;
  }

  auto * loopLatch = L.getLoopLatch();
  if (!loopLatch) {
    Report() << "remTrans: multi-latch loops not supported yet\n";
    return false;
  }

  if (loopLatch != L.getExitingBlock()) {
    Report() << "remTrans: only support latch exit loops\n";
    return false;
  }

  // only attempt loops with recognized reduction patterns
  for (auto & Inst : *L.getHeader()) {
    auto * phi = dyn_cast<PHINode>(&Inst);
    if (!phi) break;

    if (!(reda.getReductionInfo(*phi) || reda.getStrideInfo(*phi))) {
      Report() << "remTrans: unsupported header PHI " << *phi << "\n";
      return false;
    }

  }

  return true;
}

Loop*
RemainderTransform::createVectorizableLoop(Loop & L, ValueSet & uniOverrides, int vectorWidth, int tripAlign) {
// run capability checks
  // CFG caps
  if (!canTransformLoop(L)) return nullptr;

  // branch condition caps
  auto * branchCond = analyzeExitCondition(L, vectorWidth);
  if (!branchCond) {
    Report() << "remTrans: can not handle loop exit condition\n";
    L.print(outs());
#if 0
    for (auto * BB : L.blocks()) {
        outs() << "\n";
        outs() << *BB;
      }
#endif

    return nullptr;
  }

// otw, clone the scalar loop
  ValueToValueMapTy cloneMap;
  auto cloneInfo = CloneLoop(L, F, DT, PDT, LI, PB, cloneMap);
#if 0
  LoopCloner loopCloner(F, DT, PDT, LI, PB);
  ValueToValueMapTy cloneMap;
  LoopCloner::LoopCloneInfo cloneInfo = loopCloner.CloneLoop(L, cloneMap);
#endif
  auto & clonedLoop = cloneInfo.clonedLoop;

  // reda.updateForClones(LI, cloneMap);

// embed the cloned loop
  LoopTransformer loopTrans(F, DT, PDT, LI, reda, uniOverrides, *branchCond, L, clonedLoop, cloneMap, vectorWidth, tripAlign);

  // rebuild reduction information for cloned loop
  reda.analyze(clonedLoop);

  IF_DEBUG Dump(F);

  delete branchCond;

  // TODO materialize lvl call

  return &clonedLoop;
}

} // namespace rv
