//===- NatBuilder.h -----------------------------===//
//
//                     The Region Vectorizer
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//

#ifndef NATIVE_NATBUILDER_H
#define NATIVE_NATBUILDER_H


#include <vector>

#include <rv/rvInfo.h>
#include <rv/analysis/maskAnalysis.h>
#include <rv/vectorizationInfo.h>
#include <rv/PlatformInfo.h>

#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>

namespace rv {
  class Region;
}

namespace native {
  typedef std::map<const llvm::Function *, const rv::VectorMapping *> VectorMappingMap;
  typedef std::vector<llvm::Value *> LaneValueVector;
  typedef std::vector<llvm::BasicBlock *> BasicBlockVector;

  class NatBuilder {
    llvm::IRBuilder<> builder;

    rv::PlatformInfo &platformInfo;
    rv::VectorizationInfo &vectorizationInfo;
    const llvm::DominatorTree &dominatorTree;

    llvm::DataLayout layout;

    llvm::Type *i1Ty;
    llvm::Type *i32Ty;

    rv::Region *region;


  public:
    NatBuilder(rv::PlatformInfo &platformInfo, VectorizationInfo &vectorizationInfo,
               const llvm::DominatorTree &dominatorTree);

    void vectorize();

    void mapVectorValue(const llvm::Value *const value, llvm::Value *vecValue);
    void mapScalarValue(const llvm::Value *const value, llvm::Value *mapValue, unsigned laneIdx = 0);

    llvm::Value *getVectorValue(llvm::Value *const value, bool getLastBlock = false);
    llvm::Value *getScalarValue(llvm::Value *const value, unsigned laneIdx = 0);

  private:
    void vectorize(llvm::BasicBlock *const bb, llvm::BasicBlock *vecBlock);
    void vectorize(llvm::Instruction *const inst);
    void vectorizePHIInstruction(llvm::PHINode *const scalPhi);
    void vectorizeMemoryInstruction(llvm::Instruction *const inst);
    void vectorizeCallInstruction(llvm::CallInst *const scalCall);
    void vectorizeReductionCall(CallInst *rvCall, bool isRv_all);

    void copyInstruction(llvm::Instruction *const inst, unsigned laneIdx = 0);
    void copyGEPInstruction(llvm::GetElementPtrInst *const gep, unsigned laneIdx = 0);
    void copyCallInstruction(llvm::CallInst *const scalCall, unsigned laneIdx = 0);

    void fallbackVectorize(llvm::Instruction *const inst);

    void addValuesToPHINodes();

    void mapOperandsInto(llvm::Instruction *const scalInst, llvm::Instruction *inst, bool vectorizedInst,
                         unsigned laneIdx = 0);

    llvm::DenseMap<const llvm::Value *, llvm::Value *> vectorValueMap;
    std::map<const llvm::Value *, LaneValueVector> scalarValueMap;
    std::map<const llvm::BasicBlock *, BasicBlockVector> basicBlockMap;
    std::vector<llvm::PHINode *> phiVector;

    llvm::Value *requestVectorValue(llvm::Value *const value);
    llvm::Value *requestScalarValue(llvm::Value *const value, unsigned laneIdx = 0,
                                    bool skipMappingWhenDone = false);

    llvm::Value *createPTest(llvm::Value *vector, bool isRv_all);

    unsigned vectorWidth();

    bool canVectorize(llvm::Instruction *inst);
    bool shouldVectorize(llvm::Instruction *inst);

  };
}

#endif //NATIVE_NATBUILDER_H
