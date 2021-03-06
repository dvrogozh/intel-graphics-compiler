/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/

#include "Compiler/Optimizer/Scalarizer.h"
#include "Compiler/IGCPassSupport.h"
#include "GenISAIntrinsics/GenIntrinsicInst.h"

#include "common/LLVMWarningsPush.hpp"

#include "llvmWrapper/IR/Instructions.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "common/LLVMWarningsPop.hpp"
#include "common/igc_regkeys.hpp"
#include "common/Types.hpp"

using namespace llvm;
using namespace IGC;

#define V_PRINT(a,b) 

namespace VectorizerUtils{
    static void SetDebugLocBy(Instruction *I, const Instruction *setBy) {
        if (!(I->getDebugLoc())) {
            I->setDebugLoc(setBy->getDebugLoc());
        }
    }
}

// Register pass to igc-opt
#define PASS_FLAG "igc-scalarize"
#define PASS_DESCRIPTION "Scalarize functions"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(ScalarizeFunction, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_END(ScalarizeFunction, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char ScalarizeFunction::ID = 0;

ScalarizeFunction::ScalarizeFunction(bool scalarizingVectorLDSTType) : FunctionPass(ID)
{
    initializeScalarizeFunctionPass(*PassRegistry::getPassRegistry());

    for (int i = 0; i < Instruction::OtherOpsEnd; i++) m_transposeCtr[i] = 0;
    m_ScalarizingVectorLDSTType = scalarizingVectorLDSTType;

    // Initialize SCM buffers and allocation
    m_SCMAllocationArray = new SCMEntry[ESTIMATED_INST_NUM];
    m_SCMArrays.push_back(m_SCMAllocationArray);
    m_SCMArrayLocation = 0;

    V_PRINT(scalarizer, "ScalarizeFunction constructor\n");
}

ScalarizeFunction::~ScalarizeFunction()
{
    releaseAllSCMEntries();
    delete[] m_SCMAllocationArray;
    V_PRINT(scalarizer, "ScalarizeFunction destructor\n");
}

bool ScalarizeFunction::runOnFunction(Function &F)
{
    
    if (IGC_GET_FLAG_VALUE(FunctionControl) != FLAG_FCALL_FORCE_INLINE)
    {
        if (F.isDeclaration()) return false;
    }
    else
    {
        // Scalarization is done only on functions which return void (kernels)
        if (!F.getReturnType()->isVoidTy())
        {
            return false;
        }
    }

    m_currFunc = &F;
    m_moduleContext = &(m_currFunc->getContext());

    V_PRINT(scalarizer, "\nStart scalarizing function: " << m_currFunc->getName() << "\n");

    // obtain TagetData of the module
    m_pDL = &F.getParent()->getDataLayout();

    // Prepare data structures for scalarizing a new function
    m_usedVectors.clear();
    m_removedInsts.clear();
    m_SCM.clear();
    releaseAllSCMEntries();
    m_DRL.clear();

    // Scalarization. Iterate over all the instructions
    // Always hold the iterator at the instruction following the one being scalarized (so the
    // iterator will "skip" any instructions that are going to be added in the scalarization work)
    inst_iterator sI = inst_begin(m_currFunc);
    inst_iterator sE = inst_end(m_currFunc);
    while (sI != sE)
    {
        Instruction *currInst = &*sI;
        // Move iterator to next instruction BEFORE scalarizing current instruction
        ++sI;
        dispatchInstructionToScalarize(currInst);
    }

    resolveVectorValues();

    // Resolved DRL entries
    resolveDeferredInstructions();

    // Iterate over removed insts and delete them
    SmallDenseSet<Instruction*, ESTIMATED_INST_NUM>::iterator ri = m_removedInsts.begin();
    SmallDenseSet<Instruction*, ESTIMATED_INST_NUM>::iterator re = m_removedInsts.end();
    SmallDenseSet<Instruction*, ESTIMATED_INST_NUM>::iterator index = ri;

    for (;index != re; ++index)
    {
        // get rid of old users
        if (Value* val = dyn_cast<Value>(*index))
        {
            UndefValue *undefVal = UndefValue::get((*index)->getType());
            (val)->replaceAllUsesWith(undefVal);
        }
        assert((*index)->use_empty() && "Unable to remove used instruction");
        (*index)->eraseFromParent();
    }

    V_PRINT(scalarizer, "\nCompleted scalarizing function: " << m_currFunc->getName() << "\n");
    return true;
}

void ScalarizeFunction::dispatchInstructionToScalarize(Instruction *I)
{
    V_PRINT(scalarizer, "\tScalarizing Instruction: " << *I << "\n");

    if (m_removedInsts.count(I))
    {
        V_PRINT(scalarizer, "\tInstruction is already marked for removal. Being ignored..\n");
        return;
    }

    switch (I->getOpcode())
    {
        case Instruction::Add :
        case Instruction::Sub :
        case Instruction::Mul :
        case Instruction::FAdd :
        case Instruction::FSub :
        case Instruction::FMul :
        case Instruction::UDiv :
        case Instruction::SDiv :
        case Instruction::FDiv :
        case Instruction::URem :
        case Instruction::SRem :
        case Instruction::FRem :
        case Instruction::Shl :
        case Instruction::LShr :
        case Instruction::AShr :
        case Instruction::And :
        case Instruction::Or :
        case Instruction::Xor :
            scalarizeInstruction(dyn_cast<BinaryOperator>(I));
            break;
        case Instruction::ICmp :
        case Instruction::FCmp :
            scalarizeInstruction(dyn_cast<CmpInst>(I));
            break;
        case Instruction::Trunc :
        case Instruction::ZExt :
        case Instruction::SExt :
        case Instruction::FPToUI :
        case Instruction::FPToSI :
        case Instruction::UIToFP :
        case Instruction::SIToFP :
        case Instruction::FPTrunc :
        case Instruction::FPExt :
        case Instruction::PtrToInt :
        case Instruction::IntToPtr :
        case Instruction::BitCast :
            scalarizeInstruction(dyn_cast<CastInst>(I));
            break;
        case Instruction::PHI :
            scalarizeInstruction(dyn_cast<PHINode>(I));
            break;
        case Instruction::Select :
            scalarizeInstruction(dyn_cast<SelectInst>(I));
            break;
        case Instruction::ExtractElement :
            scalarizeInstruction(dyn_cast<ExtractElementInst>(I));
            break;
        case Instruction::InsertElement :
            scalarizeInstruction(dyn_cast<InsertElementInst>(I));
            break;
        case Instruction::ShuffleVector :
            scalarizeInstruction(dyn_cast<ShuffleVectorInst>(I));
            break;
        //case Instruction::Call :
        //  scalarizeInstruction(dyn_cast<CallInst>(I));
        //  break;
        case Instruction::Alloca :
            scalarizeInstruction(dyn_cast<AllocaInst>(I));
            break;
        case Instruction::GetElementPtr :
            scalarizeInstruction(dyn_cast<GetElementPtrInst>(I));
            break;
        case Instruction::Load :
            scalarizeInstruction(dyn_cast<LoadInst>(I));
            break;
        case Instruction::Store :
            scalarizeInstruction(dyn_cast<StoreInst>(I));
            break;

            // The remaining instructions are not supported for scalarization. Keep "as is"
        default :
            recoverNonScalarizableInst(I);
            break;
    }
}

void ScalarizeFunction::recoverNonScalarizableInst(Instruction *Inst)
{
    V_PRINT(scalarizer, "\t\tInstruction is not scalarizable.\n");

    // any vector value should have an SCM entry - even an empty one
    if (isa<VectorType>(Inst->getType())) getSCMEntry(Inst);

    // Iterate over all arguments. Check that they all exist (or rebuilt)
    if (CallInst *CI = dyn_cast<CallInst>(Inst))
    {
        unsigned numOperands = CI->getNumArgOperands();
        for (unsigned i = 0; i < numOperands; i++)
        {
            Value *operand = CI->getArgOperand(i);
            if (isa<VectorType>(operand->getType()))
            {
                // Recover value if needed (only needed for vector values)
                obtainVectorValueWhichMightBeScalarized(operand);
            }
        }
    }
    else
    {
        unsigned numOperands = Inst->getNumOperands();
        for (unsigned i = 0; i < numOperands; i++)
        {
            Value *operand = Inst->getOperand(i);
            if (isa<VectorType>(operand->getType()))
            {
                // Recover value if needed (only needed for vector values)
                obtainVectorValueWhichMightBeScalarized(operand);
            }
        }
    }
}

void ScalarizeFunction::scalarizeInstruction(BinaryOperator *BI)
{
    V_PRINT(scalarizer, "\t\tBinary instruction\n");
    assert(BI && "instruction type dynamic cast failed");
    VectorType *instType = dyn_cast<VectorType>(BI->getType());
    // Only need handling for vector binary ops
    if (!instType) return;

    // Prepare empty SCM entry for the instruction
    SCMEntry *newEntry = getSCMEntry(BI);

    // Get additional info from instruction
    unsigned numElements = int_cast<unsigned>(instType->getNumElements());

    // Obtain scalarized arguments
    SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>operand0;
    SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>operand1;
    bool op0IsConst, op1IsConst;

    obtainScalarizedValues(operand0, &op0IsConst, BI->getOperand(0), BI);
    obtainScalarizedValues(operand1, &op1IsConst, BI->getOperand(1), BI);

    // If both arguments are constants, don't bother Scalarizing inst
    if (op0IsConst && op1IsConst) return;

    // Generate new (scalar) instructions
    SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>newScalarizedInsts;
    newScalarizedInsts.resize(numElements);
    for (unsigned dup = 0; dup < numElements; dup++)
    {
        Value *Val = BinaryOperator::Create(
            BI->getOpcode(),
            operand0[dup],
            operand1[dup],
            BI->getName(),
            BI
            );
        if (BinaryOperator *BO = dyn_cast<BinaryOperator>(Val)) {
            // Copy overflow flags if any.
            if (isa<OverflowingBinaryOperator>(BO)) {
              BO->setHasNoSignedWrap(BI->hasNoSignedWrap());
              BO->setHasNoUnsignedWrap(BI->hasNoUnsignedWrap());
            }
            // Copy exact flag if any.
            if (isa<PossiblyExactOperator>(BO))
              BO->setIsExact(BI->isExact());
            // Copy fast math flags if any.
            if (isa<FPMathOperator>(BO))
              BO->setFastMathFlags(BI->getFastMathFlags());
        }
        newScalarizedInsts[dup] = Val;
    }

    // Add new value/s to SCM
    updateSCMEntryWithValues(newEntry, &(newScalarizedInsts[0]), BI, true);

    // Remove original instruction
    m_removedInsts.insert(BI);
}

void ScalarizeFunction::scalarizeInstruction(CmpInst *CI)
{
    V_PRINT(scalarizer, "\t\tCompare instruction\n");
    assert(CI && "instruction type dynamic cast failed");
    VectorType *instType = dyn_cast<VectorType>(CI->getType());
    // Only need handling for vector compares
    if (!instType) return;

    // Prepare empty SCM entry for the instruction
    SCMEntry *newEntry = getSCMEntry(CI);

    // Get additional info from instruction
    unsigned numElements = int_cast<unsigned>(instType->getNumElements());

    // Obtain scalarized arguments

    SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>operand0;
    SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>operand1;
    bool op0IsConst, op1IsConst;

    obtainScalarizedValues(operand0, &op0IsConst, CI->getOperand(0), CI);
    obtainScalarizedValues(operand1, &op1IsConst, CI->getOperand(1), CI);

    // If both arguments are constants, don't bother Scalarizing inst
    if (op0IsConst && op1IsConst) return;

    // Generate new (scalar) instructions
    SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>newScalarizedInsts;
    newScalarizedInsts.resize(numElements);
    for (unsigned dup = 0; dup < numElements; dup++)
    {
        newScalarizedInsts[dup] = CmpInst::Create(
            CI->getOpcode(),
            CI->getPredicate(),
            operand0[dup],
            operand1[dup],
            CI->getName(),
            CI
            );
    }

    // Add new value/s to SCM
    updateSCMEntryWithValues(newEntry, &(newScalarizedInsts[0]), CI, true);

    // Remove original instruction
    m_removedInsts.insert(CI);
}

void ScalarizeFunction::scalarizeInstruction(CastInst *CI)
{
    V_PRINT(scalarizer, "\t\tCast instruction\n");
    assert(CI && "instruction type dynamic cast failed");
    VectorType *instType = dyn_cast<VectorType>(CI->getType());

    // For BitCast - we only scalarize if src and dst types have same vector length
    if (isa<BitCastInst>(CI))
    {
        if (!instType) return recoverNonScalarizableInst(CI);
        VectorType *srcType = dyn_cast<VectorType>(CI->getOperand(0)->getType());
        if (!srcType || (instType->getNumElements() != srcType->getNumElements()))
        {
            return recoverNonScalarizableInst(CI);
        }
    }

    // Only need handling for vector cast
    if (!instType) return;

    // Prepare empty SCM entry for the instruction
    SCMEntry *newEntry = getSCMEntry(CI);

    // Get additional info from instruction
    unsigned numElements = int_cast<unsigned>(instType->getNumElements());
    assert(isa<VectorType>(CI->getOperand(0)->getType()) && "unexpected type!");
    assert(cast<VectorType>(CI->getOperand(0)->getType())->getNumElements() == numElements
        && "unexpected vector width");

    // Obtain scalarized argument
    SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>operand0;
    bool op0IsConst;

    obtainScalarizedValues(operand0, &op0IsConst, CI->getOperand(0), CI);

    // If argument is a constant, don't bother Scalarizing inst
    if (op0IsConst) return;

    // Obtain type, which ever scalar cast will cast-to
    Type *scalarDestType = instType->getElementType();

    // Generate new (scalar) instructions
    SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>newScalarizedInsts;
    newScalarizedInsts.resize(numElements);
    for (unsigned dup = 0; dup < numElements; dup++)
    {
        newScalarizedInsts[dup] = CastInst::Create(
            CI->getOpcode(),
            operand0[dup],
            scalarDestType,
            CI->getName(),
            CI
            );
    }

    // Add new value/s to SCM
    updateSCMEntryWithValues(newEntry, &(newScalarizedInsts[0]), CI, true);

    // Remove original instruction
    m_removedInsts.insert(CI);
}

void ScalarizeFunction::scalarizeInstruction(PHINode *PI)
{
    V_PRINT(scalarizer, "\t\tPHI instruction\n");
    assert(PI && "instruction type dynamic cast failed");
    VectorType *instType = dyn_cast<VectorType>(PI->getType());
    // Only need handling for vector PHI
    if (!instType) return;

    // Obtain number of incoming nodes \ PHI values
    unsigned numValues = PI->getNumIncomingValues();
    
    // Normally, a phi would be scalarized and a collection of
    // extractelements would be emitted for each value.  Since
    // VME payload CVariables don't necessarily match the size
    // of the llvm type, keep these phis vectorized here so we
    // can emit the appropriate movs in emitVectorCopy() when
    // emitting movs for phis.
    for (unsigned i=0; i < numValues; i++)
    {
        auto *Op = PI->getIncomingValue(i);

        if (auto *GII = dyn_cast<GenIntrinsicInst>(Op))
        {
            switch (GII->getIntrinsicID())
            {
            case GenISAIntrinsic::GenISA_vmeSendIME2:
            case GenISAIntrinsic::GenISA_vmeSendFBR2:
            case GenISAIntrinsic::GenISA_vmeSendSIC2:
                recoverNonScalarizableInst(PI);
                return;

            default: break;
            }
        }
    }


    // Prepare empty SCM entry for the instruction
    SCMEntry *newEntry = getSCMEntry(PI);

    // Get additional info from instruction
    Type *scalarType = instType->getElementType();
    unsigned numElements = int_cast<unsigned>(instType->getNumElements());

    // Create new (empty) PHI nodes, and place them.
    SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>newScalarizedPHI;
    newScalarizedPHI.resize(numElements);
    for (unsigned i = 0; i < numElements; i++)
    {
        newScalarizedPHI[i] = PHINode::Create(scalarType, numValues, PI->getName(), PI);
    }

    // Iterate over incoming values in vector PHI, and fill scalar PHI's accordingly
    SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>operand;

    for (unsigned j = 0; j < numValues; j++)
    {
        // Obtain scalarized arguments
        obtainScalarizedValues(operand, NULL, PI->getIncomingValue(j), PI);

        // Fill all scalarized PHI nodes with scalar arguments
        for (unsigned i = 0; i < numElements; i++)
        {
            cast<PHINode>(newScalarizedPHI[i])->addIncoming(operand[i], PI->getIncomingBlock(j));
        }
    }

    // Add new value/s to SCM
    updateSCMEntryWithValues(newEntry, &(newScalarizedPHI[0]), PI, true);

    // Remove original instruction
    m_removedInsts.insert(PI);
}

void ScalarizeFunction::scalarizeInstruction(SelectInst * SI)
{
    V_PRINT(scalarizer, "\t\tSelect instruction\n");
    assert(SI && "instruction type dynamic cast failed");
    VectorType *instType = dyn_cast<VectorType>(SI->getType());
    // Only need handling for vector select
    if (!instType) return;

    // Prepare empty SCM entry for the instruction
    SCMEntry *newEntry = getSCMEntry(SI);

    // Get additional info from instruction
    unsigned numElements = int_cast<unsigned>(instType->getNumElements());

    // Obtain scalarized arguments (select inst has 3 arguments: Cond, TrueVal, FalseVal)
    SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>condOp;
    SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>trueValOp;
    SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>falseValOp;

    obtainScalarizedValues(trueValOp, NULL, SI->getTrueValue(), SI);
    obtainScalarizedValues(falseValOp, NULL, SI->getFalseValue(), SI);

    // Check if condition is a vector.
    Value *conditionVal = SI->getCondition();
    if (isa<VectorType>(conditionVal->getType()))
    {
        // Obtain scalarized breakdowns of condition
        obtainScalarizedValues(condOp, NULL, conditionVal, SI);
    }
    else
    {
        condOp.resize(numElements);
        // Broadcast the (scalar) condition, to be used by all the insruction breakdowns
        for (unsigned i = 0; i < numElements; i++) condOp[i] = conditionVal;
    }

    // Generate new (scalar) instructions
    SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>newScalarizedInsts;
    newScalarizedInsts.resize(numElements);
    for (unsigned dup = 0; dup < numElements; dup++)
    {
        // Small optimization: Some scalar selects may be redundant (trueVal == falseVal)
        if (trueValOp[dup] != falseValOp[dup])
        {
            newScalarizedInsts[dup] = SelectInst::Create(
                condOp[dup],
                trueValOp[dup],
                falseValOp[dup],
                SI->getName(),
                SI
                );
        }
        else
        {
            // just "connect" the destination value to the true value input
            newScalarizedInsts[dup] = trueValOp[dup];
        }
    }

    // Add new value/s to SCM
    updateSCMEntryWithValues(newEntry, &(newScalarizedInsts[0]), SI, true);

    // Remove original instruction
    m_removedInsts.insert(SI);
}

void ScalarizeFunction::scalarizeInstruction(ExtractElementInst *EI)
{
    V_PRINT(scalarizer, "\t\tExtractElement instruction\n");
    assert(EI && "instruction type dynamic cast failed");

    // Proper scalarization makes "extractElement" instructions redundant
    // Only need to "follow" the scalar element (as the input vector was
    // already scalarized)
    Value *vectorValue = EI->getOperand(0);
    Value *scalarIndexVal = EI->getOperand(1);

    // If the index is not a constant - we cannot statically remove this inst
    if(!isa<ConstantInt>(scalarIndexVal)) return recoverNonScalarizableInst(EI);

    // Obtain the scalarized operands
    SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>operand;
    obtainScalarizedValues(operand, NULL, vectorValue, EI);

    // Connect the "extracted" value to all its consumers
    uint64_t scalarIndex = cast<ConstantInt>(scalarIndexVal)->getZExtValue();
    assert(NULL != operand[static_cast<unsigned int>(scalarIndex)] && "SCM error");

    // Replace all users of this inst, with the extracted scalar value
    EI->replaceAllUsesWith(operand[static_cast<unsigned int>(scalarIndex)]);

    // Remove original instruction
    m_removedInsts.insert(EI);
}

void ScalarizeFunction::scalarizeInstruction(InsertElementInst *II)
{
    V_PRINT(scalarizer, "\t\tInsertElement instruction\n");
    assert(II && "instruction type dynamic cast failed");

    // Proper scalarization makes "InsertElement" instructions redundant.
    // Only need to "follow" the scalar elements and update in SCM
    Value *sourceVectorValue = II->getOperand(0);
    Value *sourceScalarValue = II->getOperand(1);
    Value *scalarIndexVal = II->getOperand(2);

    // If the index is not a constant - we cannot statically remove this inst
    if(!isa<ConstantInt>(scalarIndexVal)) return recoverNonScalarizableInst(II);

    // Prepare empty SCM entry for the instruction
    SCMEntry *newEntry = getSCMEntry(II);

    assert(isa<ConstantInt>(scalarIndexVal) && "inst arguments error");
    uint64_t scalarIndex = cast<ConstantInt>(scalarIndexVal)->getZExtValue();
    assert(scalarIndex < dyn_cast<VectorType>(II->getType())->getNumElements() && "index error");

    // Obtain breakdown of input vector
    SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>scalarValues;
    if (isa<UndefValue>(sourceVectorValue))
    {
        // Scalarize the undef value (generate a scalar undef)
        VectorType *inputVectorType = dyn_cast<VectorType>(sourceVectorValue->getType());
        assert(inputVectorType && "expected vector argument");

        UndefValue *undefVal = UndefValue::get(inputVectorType->getElementType());

        // fill new SCM entry with UNDEFs and the new value
        scalarValues.resize(static_cast<unsigned int>(inputVectorType->getNumElements()));
        for (unsigned j = 0; j<inputVectorType->getNumElements(); j++)
        {
            scalarValues[j] = undefVal;
        }
        scalarValues[static_cast<unsigned int>(scalarIndex)] = sourceScalarValue;
    }
    else
    {
        // Obtain the scalar values of the input vector
        obtainScalarizedValues(scalarValues, NULL, sourceVectorValue, II);
        // Add the new element
        scalarValues[static_cast<unsigned int>(scalarIndex)] = sourceScalarValue;
    }

    // Add new value/s to SCM
    updateSCMEntryWithValues(newEntry, &(scalarValues[0]), II, true, false);

    // Remove original instruction
    m_removedInsts.insert(II);
}

void ScalarizeFunction::scalarizeInstruction(ShuffleVectorInst * SI)
{
    V_PRINT(scalarizer, "\t\tShuffleVector instruction\n");
    assert(SI && "instruction type dynamic cast failed");

    // Proper scalarization makes "ShuffleVector" instructions redundant.
    // Only need to "follow" the scalar elements and update in SCM

    // Grab input vectors types and width
    Value *sourceVector0Value = SI->getOperand(0);
    Value *sourceVector1Value = SI->getOperand(1);
    VectorType *inputType = dyn_cast<VectorType>(sourceVector0Value->getType());
    assert (inputType && inputType == sourceVector1Value->getType() && "vector input error");
    unsigned sourceVectorWidth = int_cast<unsigned>(inputType->getNumElements());

    // generate an array of values (pre-shuffle), which concatenates both vectors
    SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>allValues;
    allValues.resize(2*sourceVectorWidth);

    // Obtain scalarized input values (into concatenated array). if vector was Undef - keep NULL.
    if (!isa<UndefValue>(sourceVector0Value))
    {
        obtainScalarizedValues(allValues, NULL, sourceVector0Value, SI, 0);
    }
    if (!isa<UndefValue>(sourceVector1Value))
    {
        // Place values, starting in the middle of concatenated array
        obtainScalarizedValues(allValues, NULL, sourceVector1Value, SI, sourceVectorWidth);
    }

    // Generate array for shuffled scalar values
    SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>newVector;
    unsigned width = int_cast<unsigned>(SI->getType()->getNumElements());

    // Generate undef value, which may be needed as some scalar elements
    UndefValue *undef = UndefValue::get(inputType->getElementType());

    newVector.resize(width);
    // Go over shuffle order, and place scalar values in array
    for (unsigned i = 0; i < width; i++)
    {
        int maskValue = SI->getMaskValue(i);
        if (maskValue >= 0 && NULL != allValues[maskValue])
        {
            newVector[i] = allValues[maskValue];
        }
        else
        {
            newVector[i] = undef;
        }
    }

    // Create the new SCM entry
    SCMEntry *newEntry = getSCMEntry(SI);
    updateSCMEntryWithValues(newEntry, &(newVector[0]), SI, true, false);

    // Remove original instruction
    m_removedInsts.insert(SI);
}

void ScalarizeFunction::scalarizeInstruction(CallInst *CI)
{
    V_PRINT(scalarizer, "\t\tCall instruction\n");
    assert(CI && "instruction type dynamic cast failed");

    recoverNonScalarizableInst(CI);
}

void ScalarizeFunction::scalarizeInstruction(AllocaInst *AI)
{
    V_PRINT(scalarizer, "\t\tAlloca instruction\n");
    assert(AI && "instruction type dynamic cast failed");

    return recoverNonScalarizableInst(AI);
}

void ScalarizeFunction::scalarizeInstruction(GetElementPtrInst *GI)
{
    V_PRINT(scalarizer, "\t\tGEP instruction\n");
    assert(GI && "instruction type dynamic cast failed");

    return recoverNonScalarizableInst(GI);
}

void ScalarizeFunction::scalarizeInstruction(LoadInst *LI)
{
    V_PRINT(scalarizer, "\t\tLoad instruction\n");
    assert(LI && "instruction type dynamic cast failed");

    VectorType *dataType = dyn_cast<VectorType>(LI->getType());
    if (isScalarizableLoadStoreType(dataType) && m_pDL)
    {
        // Prepare empty SCM entry for the instruction
        SCMEntry *newEntry = getSCMEntry(LI);

        // Get additional info from instruction
        unsigned int vectorSize = int_cast<unsigned int>(m_pDL->getTypeAllocSize(dataType));
        unsigned int elementSize = int_cast<unsigned int>(m_pDL->getTypeSizeInBits(dataType->getElementType()) / 8);
        assert((vectorSize/elementSize > 0) && (vectorSize % elementSize == 0) &&
            "vector size should be a multiply of element size");
        unsigned numDupElements = int_cast<unsigned>(dataType->getNumElements());

        // Obtain scalarized arguments
// 1 - to allow scalarizing Load with any pointer type
// 0 - to limit scalarizing to special case where packetizer benifit from the scalarizing
#if 1
        // Apply the bit-cast on the GEP base and add base-offset then fix the index by multiply it with numElements. (assuming one index only).
        Value *GepPtr = LI->getOperand(0);
        PointerType *GepPtrType = cast<PointerType>(GepPtr->getType());
        Value *operandBase = BitCastInst::CreatePointerCast(GepPtr, dataType->getScalarType()->getPointerTo(GepPtrType->getAddressSpace()), "ptrVec2ptrScl", LI);
        Type * indexType = Type::getInt32Ty(*m_moduleContext);
        // Generate new (scalar) instructions
        SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>newScalarizedInsts;
        newScalarizedInsts.resize(numDupElements);
        for (unsigned dup = 0; dup < numDupElements; dup++)
        {
            Constant *laneVal = ConstantInt::get(indexType, dup);
            Value *pGEP = GetElementPtrInst::Create(nullptr, operandBase, laneVal, "GEP_lane", LI);
            newScalarizedInsts[dup] = new LoadInst(pGEP, LI->getName(), LI);
        }
#else
        GetElementPtrInst *operand = dyn_cast<GetElementPtrInst>(LI->getOperand(0));
        if (!operand || operand->getNumIndices() != 1)
        {
            return recoverNonScalarizableInst(LI);
        }
        // Apply the bit-cast on the GEP base and add base-offset then fix the index by multiply it with numElements. (assuming one index only).
        Value *GepPtr = operand->getPointerOperand();
        PointerType *GepPtrType = cast<PointerType>(GepPtr->getType());
        Value *operandBase = BitCastInst::CreatePointerCast(GepPtr, dataType->getScalarType()->getPointerTo(GepPtrType->getAddressSpace()), "ptrVec2ptrScl", LI);
        Type * indexType = operand->getOperand(1)->getType();
        // Generate new (scalar) instructions
        Value *newScalarizedInsts[MAX_INPUT_VECTOR_WIDTH];
        Constant *elementNumVal = ConstantInt::get(indexType, numElements);
        for (unsigned dup = 0; dup < numDupElements; dup++)
        {
            Constant *laneVal = ConstantInt::get(indexType, dup);
            Value *pGEP = GetElementPtrInst::Create(operandBase, laneVal, "GEP_lane", LI);
            Value *pIndex = BinaryOperator::CreateMul(operand->getOperand(1), elementNumVal, "GEPIndex_s", LI);
            pGEP = GetElementPtrInst::Create(pGEP, pIndex, "GEP_s", LI);
            newScalarizedInsts[dup] = new LoadInst(pGEP, LI->getName(), LI);
        }
#endif
        // Add new value/s to SCM
        updateSCMEntryWithValues(newEntry, &(newScalarizedInsts[0]), LI, true);

        // Remove original instruction
        m_removedInsts.insert(LI);
        return;
    }
    return recoverNonScalarizableInst(LI);
}

void ScalarizeFunction::scalarizeInstruction(StoreInst *SI)
{
    V_PRINT(scalarizer, "\t\tStore instruction\n");
    assert(SI && "instruction type dynamic cast failed");

    int indexPtr = SI->getPointerOperandIndex();
    int indexData = 1-indexPtr;
    VectorType *dataType = dyn_cast<VectorType>(SI->getOperand(indexData)->getType());
    if (isScalarizableLoadStoreType(dataType) && m_pDL)
    {
        // Get additional info from instruction
        unsigned int vectorSize = int_cast<unsigned int>(m_pDL->getTypeAllocSize(dataType));
        unsigned int elementSize = int_cast<unsigned int>(m_pDL->getTypeSizeInBits(dataType->getElementType()) / 8);
        assert((vectorSize/elementSize > 0) && (vectorSize % elementSize == 0) &&
            "vector size should be a multiply of element size");

        unsigned numDupElements = int_cast<unsigned>(dataType->getNumElements());

        // Obtain scalarized arguments
// 1 - to allow scalarizing Load with any pointer type
// 0 - to limit scalarizing to special case where packetizer benifit from the scalarizing
#if 1
        SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>operand0;

        bool opIsConst;
        obtainScalarizedValues(operand0, &opIsConst, SI->getOperand(indexData), SI);

        // Apply the bit-cast on the GEP base and add base-offset then fix the index by multiply it with numElements. (assuming one index only).
        Value *GepPtr = SI->getOperand(indexPtr);
        PointerType *GepPtrType = cast<PointerType>(GepPtr->getType());
        Value *operandBase = BitCastInst::CreatePointerCast(GepPtr, dataType->getScalarType()->getPointerTo(GepPtrType->getAddressSpace()), "ptrVec2ptrScl", SI);
        Type * indexType = Type::getInt32Ty(*m_moduleContext);
        // Generate new (scalar) instructions
        for (unsigned dup = 0; dup < numDupElements; dup++)
        {
            Constant *laneVal = ConstantInt::get(indexType, dup);
            Value *pGEP = GetElementPtrInst::Create(nullptr, operandBase, laneVal, "GEP_lane", SI);
            new StoreInst(operand0[dup], pGEP, SI);
        }
#else
        GetElementPtrInst *operand1 = dyn_cast<GetElementPtrInst>(SI->getOperand(indexPtr));
        if (!operand1 || operand1->getNumIndices() != 1)
        {
            return recoverNonScalarizableInst(SI);
        }
        Value *operand0[MAX_INPUT_VECTOR_WIDTH];
        bool opIsConst;
        obtainScalarizedValues(operand0, &opIsConst, SI->getOperand(indexData), SI);

        // Apply the bit-cast on the GEP base and add base-offset then fix the index by multiply it with numElements. (assuming one index only).
        Value *GepPtr = operand1->getPointerOperand();
        PointerType *GepPtrType = cast<PointerType>(GepPtr->getType());
        Value *operandBase = BitCastInst::CreatePointerCast(GepPtr, dataType->getScalarType()->getPointerTo(GepPtrType->getAddressSpace()), "ptrVec2ptrScl", SI);
        Type * indexType = operand1->getOperand(1)->getType();
        // Generate new (scalar) instructions
        Constant *elementNumVal = ConstantInt::get(indexType, numElements);
        for (unsigned dup = 0; dup < numDupElements; dup++)
        {
            Constant *laneVal = ConstantInt::get(indexType, dup);
            Value *pGEP = GetElementPtrInst::Create(operandBase, laneVal, "GEP_lane", SI);
            Value *pIndex = BinaryOperator::CreateMul(operand1->getOperand(1), elementNumVal, "GEPIndex_s", SI);
            pGEP = GetElementPtrInst::Create(pGEP, pIndex, "GEP_s", SI);
            new StoreInst(operand0[dup], pGEP, SI);
        }
#endif
        // Remove original instruction
        m_removedInsts.insert(SI);
        return;
    }
    return recoverNonScalarizableInst(SI);
}

void ScalarizeFunction::obtainScalarizedValues(SmallVectorImpl<Value *> &retValues, bool *retIsConstant,
                                               Value *origValue, Instruction *origInst, int destIdx)
{
    V_PRINT(scalarizer, "\t\t\tObtaining scalar value... " << *origValue << "\n");

    VectorType *origType = dyn_cast<VectorType>(origValue->getType());
    assert(origType && "Value must have a vector type!");
    unsigned width = int_cast<unsigned>(origType->getNumElements());

    if (destIdx == -1)
    {
        destIdx = 0;
        retValues.resize(width);
    }

    if (NULL != retIsConstant)
    {
        // Set retIsConstant (return value) to true, if the origValue is constant
        if (!isa<Constant>(origValue)) 
        {
            *retIsConstant = false;
        }
        else
        {
            *retIsConstant = true;
        }
    }

    // Lookup value in SCM
    SCMEntry *currEntry = getScalarizedValues(origValue);
    if (currEntry && (NULL != currEntry->scalarValues[0]))
    {
        // Value was found in SCM
        V_PRINT(scalarizer,
                "\t\t\tFound existing entry in lookup of " << origValue->getName() << "\n");
        for (unsigned i = 0; i < width; i++)
        {
            // Copy values to return array
            assert(NULL != currEntry->scalarValues[i] && "SCM entry contains NULL value");
            retValues[i + destIdx] = currEntry->scalarValues[i];
        }
    }
    else if (isa<UndefValue>(origValue))
    {
        assert(origType && "original value must have a vector type!");
        // value is an undefVal. Break it to element-sized undefs
        V_PRINT(scalarizer, "\t\t\tUndefVal constant\n");
        Value *undefElement = UndefValue::get(origType->getElementType());
        for (unsigned i = 0; i < width; i++)
        {
            retValues[i + destIdx] = undefElement;
        }
    }
    else if (Constant *vectorConst = dyn_cast<Constant>(origValue))
    {
        V_PRINT(scalarizer, "\t\t\tProper constant: " <<    *vectorConst << "\n");
        // Value is a constant. Break it down to scalars by employing a constant expression
        for (unsigned i = 0; i < width; i++)
        {
            retValues[i + destIdx] = ConstantExpr::getExtractElement(vectorConst,
                ConstantInt::get(Type::getInt32Ty(context()), i));
        }
    }
    else if (isa<Instruction>(origValue) && !currEntry)
    {
        // Instruction not found in SCM. Means it will be defined in a following basic block.
        // Generate a DRL: dummy values, which will be resolved after all scalarization is complete.
        V_PRINT(scalarizer, "\t\t\t*** Not found. Setting DRL. \n");
        Type *dummyType = origType->getElementType();
        V_PRINT(scalarizer, "\t\tCreate Dummy Scalar value/s (of type " << *dummyType << ")\n");
        Constant *dummyPtr = ConstantPointerNull::get(dummyType->getPointerTo());
        DRLEntry newDRLEntry;
        newDRLEntry.unresolvedInst = origValue;
        newDRLEntry.dummyVals.resize(width);
        for (unsigned i = 0; i < width; i++)
        {
            // Generate dummy "load" instruction (but don't really place in function)
            retValues[i + destIdx] = new LoadInst(dummyPtr);
            newDRLEntry.dummyVals[i] = retValues[i + destIdx];
        }
        // Copy the data into DRL structure
        m_DRL.push_back(newDRLEntry);
    }
    else
    {
        V_PRINT(scalarizer,
                "\t\t\tCreating scalar conversion for " << origValue->getName() << "\n");
        // Value is an Instruction/global/function argument, and was not converted to scalars yet.
        // Create scalar values (break down the vector) and place in SCM:
        //   %scalar0 = extractelement <4 x Type> %vector, i32 0
        //   %scalar1 = extractelement <4 x Type> %vector, i32 1
        //   %scalar2 = extractelement <4 x Type> %vector, i32 2
        //   %scalar3 = extractelement <4 x Type> %vector, i32 3
        // The breaking instructions will be placed the the head of the function, or right
        // after the instruction (if it is an instruction)
        Instruction *locationInst = &*(inst_begin(m_currFunc));
        Instruction *origInstruction = dyn_cast<Instruction>(origValue);
        if (origInstruction)
        {
            BasicBlock::iterator insertLocation(origInstruction);
            ++insertLocation;
            locationInst = &(*insertLocation);
            // If the insert location is PHI, move the insert location to after all PHIs is the block
            if (isa<PHINode>(locationInst))
            {
                locationInst = locationInst->getParent()->getFirstNonPHI();
            }
        }

        // Generate extractElement instructions
        for (unsigned i = 0; i < width; ++i)
        {
            Value *constIndex = ConstantInt::get(Type::getInt32Ty(context()), i);
            retValues[i + destIdx] = ExtractElementInst::Create(origValue, constIndex, "scalar", locationInst);
        }
        SCMEntry *newEntry = getSCMEntry(origValue);
        updateSCMEntryWithValues(newEntry, &(retValues[destIdx]), origValue, false);

    }
}

void ScalarizeFunction::obtainVectorValueWhichMightBeScalarized(Value * vectorVal)
{
    m_usedVectors.insert(vectorVal);
}

void ScalarizeFunction::resolveVectorValues()
{
    SmallSetVector<Value *, ESTIMATED_INST_NUM>::iterator it = m_usedVectors.begin();
    SmallSetVector<Value *, ESTIMATED_INST_NUM>::iterator e = m_usedVectors.end();
    for (; it !=e ; ++it){
        obtainVectorValueWhichMightBeScalarizedImpl(*it);
    }
}

void ScalarizeFunction::obtainVectorValueWhichMightBeScalarizedImpl(Value * vectorVal)
{
    assert(isa<VectorType>(vectorVal->getType()) && "Must be a vector type");
    if (isa<UndefValue>(vectorVal)) return;

    // ONLY IF the value appears in the SCM - there is a chance it was removed.
    if (!m_SCM.count(vectorVal)) return;
    SCMEntry *valueEntry = m_SCM[vectorVal];

    // Check in SCM entry, if value was really removed
    if (false == valueEntry->isOriginalVectorRemoved) return;

    V_PRINT(scalarizer, "\t\t\tTrying to use a removed value. Reassembling it...\n");
    // The vector value was removed. Need to reassemble it...
    //   %assembled.vect.0 = insertelement <4 x type> undef             , type %scalar.0, i32 0
    //   %assembled.vect.1 = insertelement <4 x type> %indx.vect.0, type %scalar.1, i32 1
    //   %assembled.vect.2 = insertelement <4 x type> %indx.vect.1, type %scalar.2, i32 2
    //   %assembled.vect.3 = insertelement <4 x type> %indx.vect.2, type %scalar.3, i32 3
    // Place the re-assembly in the location where the original instruction was
    Instruction *vectorInst = dyn_cast<Instruction>(vectorVal);
    assert(vectorInst && "SCM reports a non-instruction was removed. Should not happen");
    Instruction *insertLocation = vectorInst;
    // If the original instruction was PHI, place the re-assembly only after all PHIs is the block
    if (isa<PHINode>(vectorInst))
    {
        insertLocation = insertLocation->getParent()->getFirstNonPHI();
    }

    Value *assembledVector = UndefValue::get(vectorVal->getType());
    unsigned width = int_cast<unsigned>(dyn_cast<VectorType>(vectorVal->getType())->getNumElements());
    for (unsigned i = 0; i < width; i++)
    {
        assert(NULL != valueEntry->scalarValues[i] && "SCM entry has NULL value");
        Value *constIndex = ConstantInt::get(Type::getInt32Ty(context()), i);
        Instruction *insert = InsertElementInst::Create(assembledVector,
            valueEntry->scalarValues[i], constIndex, "assembled.vect", insertLocation);
        VectorizerUtils::SetDebugLocBy(insert, vectorInst);
        assembledVector = insert;
        V_PRINT(scalarizer,
                "\t\t\tCreated vector assembly inst:" << *assembledVector << "\n");
    }
    // Replace the uses of "vectorVal" with the new vector
    vectorVal->replaceAllUsesWith(assembledVector);

    // create SCM entry to represent the new vector value..
    SCMEntry *newEntry = getSCMEntry(assembledVector);
    updateSCMEntryWithValues(newEntry, &(valueEntry->scalarValues[0]), assembledVector, false);
}

ScalarizeFunction::SCMEntry *ScalarizeFunction::getSCMEntry(Value *origValue)
{
    // origValue may be scalar or vector:
    // When the actual returned value of the CALL inst is different from the The "proper" retval
    // the original CALL inst value may be scalar (i.e. int2 is converted to double which is a scalar)
    assert(!isa<UndefValue>(origValue) && "Trying to create SCM to undef value...");
    if (m_SCM.count(origValue)) return m_SCM[origValue];

    // If index of next free SCMEntry overflows the array size, create a new array
    if (m_SCMArrayLocation == ESTIMATED_INST_NUM)
    {
        // Create new SCMAllocationArray, push it to the vector of arrays, and set free index to 0
        m_SCMAllocationArray = new SCMEntry[ESTIMATED_INST_NUM];
        m_SCMArrays.push_back(m_SCMAllocationArray);
        m_SCMArrayLocation = 0;
    }
    // Allocate the new entry, and increment the free-element index
    SCMEntry *newEntry = &(m_SCMAllocationArray[m_SCMArrayLocation++]);

    // Set all primary data in entry
    if(newEntry->scalarValues.size())
        newEntry->scalarValues[0] = NULL;
    else
        newEntry->scalarValues.push_back(NULL);

    newEntry->isOriginalVectorRemoved = false;

    // Insert new entry to SCM map
    m_SCM.insert(std::pair<Value*, SCMEntry*>(origValue, newEntry));

    return newEntry;
}

void ScalarizeFunction::updateSCMEntryWithValues(ScalarizeFunction::SCMEntry *entry,
                                                 Value *scalarValues[],
                                                 const Value *origValue,
                                                 bool isOrigValueRemoved,
                                                 bool matchDbgLoc)
{
    assert((origValue->getType()->isArrayTy() ||    origValue->getType()->isVectorTy()) &&
        "only Vector values are supported");
    unsigned width = int_cast<unsigned>(dyn_cast<VectorType>(origValue->getType())->getNumElements());

    entry->isOriginalVectorRemoved = isOrigValueRemoved;

    entry->scalarValues.resize(width);

    for (unsigned i = 0; i < width; ++i)
    {
        assert(NULL != scalarValues[i] && "Trying to fill SCM with NULL value");
        entry->scalarValues[i] = scalarValues[i];
    }

    if (matchDbgLoc)
    {
        if (const Instruction *origInst = dyn_cast<Instruction>(origValue))
        {
            for (unsigned i = 0; i < width; ++i)
            {
                Instruction *scalarInst = dyn_cast<Instruction>(scalarValues[i]);
                if (scalarInst) VectorizerUtils::SetDebugLocBy(scalarInst, origInst);
            }
        }
    }
}

ScalarizeFunction::SCMEntry *ScalarizeFunction::getScalarizedValues(Value *origValue)
{
    if (m_SCM.count(origValue)) return m_SCM[origValue];
    return NULL;
}

void ScalarizeFunction::releaseAllSCMEntries()
{
    assert(m_SCMArrays.size() > 0 && "At least one buffer is allocated at all times");
    while (m_SCMArrays.size() > 1)
    {
        // If there are additional allocated entry Arrays, release all of them (leave only the first)
        SCMEntry *popEntry = m_SCMArrays.pop_back_val();
        delete[] popEntry;
    }
    // set the "current" array pointer to the only remaining array
    m_SCMAllocationArray = m_SCMArrays[0];
    m_SCMArrayLocation = 0;
}

void ScalarizeFunction::resolveDeferredInstructions()
{
    // lambda to check if a value is a dummy instruction
    auto isDummyValue = [] (Value* val)->bool
    {
        LoadInst* ld = dyn_cast<LoadInst>(val);
        return (ld && isa<ConstantPointerNull>(ld->getPointerOperand()));
    };

    for (unsigned index = 0; index < m_DRL.size(); ++index)
    {
        DRLEntry current = m_DRL[index];
        V_PRINT(scalarizer,
                "\tDRL Going to fix value of orig inst: " << *current.unresolvedInst << "\n");
        Instruction *vectorInst = dyn_cast<Instruction>(current.unresolvedInst);
        assert(vectorInst && "DRL only handles unresolved instructions");

        VectorType *currType = dyn_cast<VectorType>(vectorInst->getType());
        assert(currType && "Cannot have DRL of non-vector value");
        unsigned width = int_cast<unsigned>(currType->getNumElements());

        SCMEntry *currentInstEntry = getSCMEntry(vectorInst);

        bool hasDummyLoad = false;
        bool scalarsInitialized = (currentInstEntry->scalarValues[0] != NULL);

        // Check if the instruction has been fully scalarized
        if (scalarsInitialized)
        {
            for (unsigned i = 0; i < width; i++)
            {
                if (isDummyValue(currentInstEntry->scalarValues[i]))
                {
                    hasDummyLoad = true;
                    break;
                }
            }
        }

        if (!scalarsInitialized || hasDummyLoad)
        {
            V_PRINT(scalarizer, "\t\tInst was not scalarized yet, Scalarizing now...\n");
            SmallVector<Value *, MAX_INPUT_VECTOR_WIDTH>newInsts;

            // This instruction was not scalarized. Create scalar values and place in SCM.
            //   %scalar0 = extractelement <4 x Type> %vector, i32 0
            //   %scalar1 = extractelement <4 x Type> %vector, i32 1
            //   %scalar2 = extractelement <4 x Type> %vector, i32 2
            //   %scalar3 = extractelement <4 x Type> %vector, i32 3
            // Place the vector break-down instructions right after the actual vector
            BasicBlock::iterator insertLocation(vectorInst);
            ++insertLocation;
            // If the insert location is PHI, move the insert location to after all PHIs is the block
            if (isa<PHINode>(insertLocation))
            {
                insertLocation = BasicBlock::iterator(insertLocation->getParent()->getFirstNonPHI());
            }

            newInsts.resize(width);
            for (unsigned i = 0; i < width; i++)
            {
                if (!scalarsInitialized || isDummyValue(currentInstEntry->scalarValues[i]))
                {
                    Value *constIndex = ConstantInt::get(Type::getInt32Ty(context()), i);
                    Instruction *EE = ExtractElementInst::Create(vectorInst, constIndex, "scalar", &(*insertLocation));
                    newInsts[i] = EE;
                }
                else
                {
                    newInsts[i] = currentInstEntry->scalarValues[i];
                }
            }
            updateSCMEntryWithValues(currentInstEntry, &(newInsts[0]), vectorInst, false);
        }

        // Connect the resolved values to their consumers
        for (unsigned i = 0; i < width; ++i)
        {
            Instruction *dummyInst = dyn_cast<Instruction>(current.dummyVals[i]);
            assert(dummyInst && "Dummy values are all instructions!");
            Value* scalarVal = currentInstEntry->scalarValues[i];
            dummyInst->replaceAllUsesWith(scalarVal);
            IGCLLVM::DeleteInstruction(dummyInst);
        }
    }

    // clear DRL
    m_DRL.clear();
}

bool ScalarizeFunction::isScalarizableLoadStoreType(VectorType *type)
{
    // Scalarize Load/Store worth doing only if:
    //  1. Gather/Scatter are supported
    //  2. Load/Store type is a vector
    return (m_ScalarizingVectorLDSTType && (NULL != type));
}

extern "C" FunctionPass* createScalarizerPass(bool scalarizingVectorLDSTType)
{
    return new ScalarizeFunction(scalarizingVectorLDSTType);
}


