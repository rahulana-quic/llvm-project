#include "llvm/Transforms/IPO/StructureLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TypeSize.h"

#define DEBUG_TYPE "structurelayout"

using namespace llvm;

// The main analysis for the optimization. For each allocation, we inspect
// every access inside that chunk of memory
bool StructureLayout::handleAllocation(Instruction *I, GEPInfoT &GEPInfo) {
  switch(I->getOpcode()) {

  case Instruction::Load: {
    uint64_t LoadSize = DL->getTypeStoreSize(I->getType()).getFixedValue();

    if(GEPInfo.Level > 0) {
      APInt CurrentConstantOffset = GEPInfo.ConstantOffset.back();

      LLVM_DEBUG(dbgs() << "[SLO] Offset: " << CurrentConstantOffset << " Load size: " << LoadSize << "\n");

      // We collect the size from an offset the first time it is accessed
      if(!OffsetSizeMap[CurrentConstantOffset]) {
        OffsetSizeMap[CurrentConstantOffset] = LoadSize;
        OffsetInstructionsMap[CurrentConstantOffset].emplace_back(I);
        InstructionIndexMap[I] = GEPInfo.Index;
      }
      else {
        // If there was a memory access at this offset previously,
        // any subsequent access must be of the same size
        if(OffsetSizeMap[CurrentConstantOffset] != LoadSize) {
          LLVM_DEBUG(dbgs() << "[SLO] Error: Load-Store sizes to offset do not match\n");
          return false;
        }
      }
    }
    
    break;
  }

  case Instruction::GetElementPtr: {
    auto *GEP = cast<GetElementPtrInst>(I);

    unsigned BitWidth = DL->getIndexTypeSizeInBits(GEP->getType());
    MapVector<Value*, APInt> VariableOffsets;
    APInt ConstantOffset(BitWidth, 0);
  
    GEP->collectOffset(*DL, BitWidth, VariableOffsets, ConstantOffset);
    
    // We assume that the GEPs are canonicalized and there exists only
    // one GEP per allocation that has the struct type. The index of the
    // GEP is collected too
    if(!VariableOffsets.empty()) {
      APInt CurrentGEPMultiplier = VariableOffsets.front().second;
      if(GEPInfo.Multiplier.isZero()) {
        GEPInfo.Multiplier = CurrentGEPMultiplier;
        GEPInfo.Index = GEP->getOperand(1);
      }
      else {
        // Every GEP that is a user of an allocation should have the same
        // type (the struct)
        if(GEPInfo.Multiplier != CurrentGEPMultiplier) {
          LLVM_DEBUG(dbgs() << "[SLO] Error: Unsuitable offset access\n");
          return false;
        }
        if(GEPInfo.Index != GEP->getOperand(1))
          GEPInfo.Index = GEP->getOperand(1);
      }
    }
    
    // The level is used to differentiate between a user of an allocation
    // and user of a GEP
    // FIXME: Can this be a bool?
    GEPInfo.Level++;

    // The ConstantOffsets need to be in a stack because a GEP with 0 offset
    // could be used by another GEP for further calculations as well as a load
    // or a store. We need to use the corresponding offset at the appropriate
    // GEPLevel
    GEPInfo.ConstantOffset.push_back(ConstantOffset);

    // Recursive analysis on the users of this GEP
    for(User *U : I->users())
      if(!handleAllocation(dyn_cast<Instruction>(U), GEPInfo))
        return false;

    GEPInfo.ConstantOffset.pop_back();
    GEPInfo.Level--;

    break;
  }

  case Instruction::Store: {
    // This is similar to the analysis of Load instructions
    StoreInst* Store = cast<StoreInst>(I);
    uint64_t StoreSize = DL->getTypeStoreSize(Store->getValueOperand()->getType()).getFixedValue();

    if(GEPInfo.Level > 0) {
      APInt CurrentConstantOffset = GEPInfo.ConstantOffset.back();
      
      LLVM_DEBUG(dbgs() << "[SLO] Offset: " << CurrentConstantOffset << " Store size: " << StoreSize << "\n");
    
      if(!OffsetSizeMap[CurrentConstantOffset]) {
        OffsetSizeMap[CurrentConstantOffset] = StoreSize;
      }
      else {
        if(OffsetSizeMap[CurrentConstantOffset] != StoreSize) {
          LLVM_DEBUG(dbgs() << "[SLO] Error: Load-Store sizes to offset do not match\n");
          return false;
        }
      }
      OffsetInstructionsMap[CurrentConstantOffset].emplace_back(I);
      InstructionIndexMap[I] = GEPInfo.Index;
    }
    break;
  }
  }
  return true;
}

bool isAllocation(Instruction &I) {
  if(auto *CB = dyn_cast<CallBase>(&I))
    if(CB->getCalledFunction()->getName() == "malloc")
      return true;

  return false;
}

// This is the transformation step. The mallocs and non GEP users are left untouched
//
// FIXME: Handle mallocs with different forms. Currently this only handles mallocs in
// the following form:
//    %0 = load i32* @num
//    %conv = sext i32 %0 to i64
//    %mul = shl nsw i64 %conv, 3
//    %call = call noalias i8* @malloc(i64 %mul)
//
// FIXME: Rewrite the size of the malloc when there are unused members of the struct

void StructureLayout::rewriteAllocation(Instruction *I) {
  CallInst *Malloc = cast<CallInst>(I);
  LLVMContext &Context = Malloc->getContext();
  Instruction *Mul = cast<Instruction>(Malloc->getOperand(0));
  Instruction *NumOfElements = cast<Instruction>(Mul->getOperand(0));

  // This accumulates the sizes of the elements for which a new GEP has been generated
  unsigned AccumulatedSize = 0;

  // Currently the order which new instructions are generated and the offset
  for(auto OffsetSizeMapElement : OffsetSizeMap) {
    ConstantInt *Size = ConstantInt::get(Context, APInt(64, OffsetSizeMapElement.second, false));
    
    for(auto I : OffsetInstructionsMap[OffsetSizeMapElement.first]) {
      LLVM_DEBUG(dbgs() << "[SLO] Rewriting: " << *I << "\n");
      Value *CurrentOffset, *Index, *NewGEP;
      
      // This handles the case of a new instruction being generated for elements at non zero
      // offsets. The accumulated size is used as the first offset and a GEP is generated to return
      // a pointer to the start of the memory location from which the current element will
      // be reordered. Another GEP is generated which will offset the pointer with respect
      // to the previous GEP
      if(AccumulatedSize != 0) {
        ConstantInt *AccumulatedSizeConstant = ConstantInt::get(Context, APInt(64, AccumulatedSize, false));
        Value *AccumulatedOffset = BinaryOperator::CreateMul(NumOfElements, AccumulatedSizeConstant, "", I);
        Value *AccumulatedOffsetGEP = GetElementPtrInst::Create(Type::getInt8Ty(Context), Malloc, {AccumulatedOffset}, "", I);
        Index = BinaryOperator::CreateMul(InstructionIndexMap[I], Size, "", I);
        NewGEP = GetElementPtrInst::Create(Type::getInt8Ty(Context), AccumulatedOffsetGEP, {Index}, "", I);
      }

      // In the case of new instructions being generated for elements starting from the zero offset
      // of the original allocation, multiplying the index by the size suffices
      else {
        CurrentOffset = BinaryOperator::CreateMul(InstructionIndexMap[I], Size, "", I);
        NewGEP = GetElementPtrInst::Create(Type::getInt8Ty(Context), Malloc, {CurrentOffset}, "", I);
      }

      Instruction *NewI = I->clone();
      if(LoadInst *LI = dyn_cast<LoadInst>(I))
        NewI->setOperand(0, NewGEP);
      else
        NewI->setOperand(1, NewGEP);

      NewI->insertBefore(I);

      DeadInstructions.emplace_back(I);
      if(LoadInst *LI = dyn_cast<LoadInst>(I)) {
        LI->replaceAllUsesWith(NewI);
        DeadInstructions.emplace_back(LI->getPointerOperand());
        LI->getPointerOperand()->replaceAllUsesWith(NewGEP);
      }
      else {
        I->replaceAllUsesWith(NewI);
        DeadInstructions.emplace_back(dyn_cast<StoreInst>(I)->getPointerOperand());
        dyn_cast<StoreInst>(I)->getPointerOperand()->replaceAllUsesWith(NewGEP);
      }

    }
    AccumulatedSize += Size->getZExtValue();
  }
}

void StructureLayout::run(Module &M) {
  DL = &M.getDataLayout();
  GEPInfoT GEPInfo;
  bool canRewriteAllocation;
  
  for(Function &F : M)
    for(BasicBlock &BB : F)
      for(Instruction &I : BB) {
        canRewriteAllocation = true;
        if(isAllocation(I)) { 
          for(User *U : I.users()) { 
            // Analysis
            if(!handleAllocation(dyn_cast<Instruction>(U), GEPInfo)) {
              canRewriteAllocation = false;
              break;
            }
          }
          
          // Transformation
          if(canRewriteAllocation)
            rewriteAllocation(dyn_cast<Instruction>(&I));
          else
            break;
        }
      }
        
  // Cleanup
  for(auto I : DeadInstructions)
    cast<Instruction>(I)->eraseFromParent();
}

PreservedAnalyses StructureLayoutPass::run(Module &M, ModuleAnalysisManager &AM) {
  StructureLayout SL;
  SL.run(M);
  return PreservedAnalyses::all();
}
