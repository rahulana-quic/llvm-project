#ifndef LLVM_TRANSFORMS_IPO_STRUCTURELAYOUT_H
#define LLVM_TRANSFORMS_IPO_STRUCTURELAYOUT_H

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/ValueHandle.h"

namespace llvm {

struct GEPInfoT {
  APInt Multiplier;
  SmallVector<APInt, 4> ConstantOffset;
  Value *Index;
  unsigned Level;

  GEPInfoT() : Level(0) {}
};

class StructureLayout {
  private:
    const DataLayout *DL;
    bool handleAllocation(Instruction *I, GEPInfoT& GEPInfo);
    void rewriteAllocation(Instruction *I);
    MapVector<APInt, uint64_t> OffsetSizeMap;
    MapVector<APInt, SmallVector<Instruction*, 8>> OffsetInstructionsMap;
    MapVector<Instruction*, Value*> InstructionIndexMap;
    SmallVector<Value*, 4> DeadInstructions;
  public:
    void run(Module &M);

};

class StructureLayoutPass : public PassInfoMixin<StructureLayoutPass> {
public:

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_IPO_STRUCTURELAYOUT_H
