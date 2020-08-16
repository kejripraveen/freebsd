//===- MipsRegisterBankInfo.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// This file declares the targeting of the RegisterBankInfo class for Mips.
/// \todo This should be generated by TableGen.
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MIPS_MIPSREGISTERBANKINFO_H
#define LLVM_LIB_TARGET_MIPS_MIPSREGISTERBANKINFO_H

#include "llvm/CodeGen/GlobalISel/RegisterBankInfo.h"

#define GET_REGBANK_DECLARATIONS
#include "MipsGenRegisterBank.inc"

namespace llvm {

class TargetRegisterInfo;

class MipsGenRegisterBankInfo : public RegisterBankInfo {
#define GET_TARGET_REGBANK_CLASS
#include "MipsGenRegisterBank.inc"
};

/// This class provides the information for the target register banks.
class MipsRegisterBankInfo final : public MipsGenRegisterBankInfo {
public:
  MipsRegisterBankInfo(const TargetRegisterInfo &TRI);

  const RegisterBank &getRegBankFromRegClass(const TargetRegisterClass &RC,
                                             LLT) const override;

  const InstructionMapping &
  getInstrMapping(const MachineInstr &MI) const override;

  /// Here we have to narrowScalar s64 operands to s32, combine away G_MERGE or
  /// G_UNMERGE and erase instructions that became dead in the process. We
  /// manually assign bank to def operand of all new instructions that were
  /// created in the process since they will not end up in RegBankSelect loop.
  void applyMappingImpl(const OperandsMapper &OpdMapper) const override;

  /// RegBankSelect determined that s64 operand is better to be split into two
  /// s32 operands in gprb. Here we manually set register banks of def operands
  /// of newly created instructions since they will not get regbankselected.
  void setRegBank(MachineInstr &MI, MachineRegisterInfo &MRI) const;

private:
  /// Some instructions are used with both floating point and integer operands.
  /// We assign InstType to such instructions as it helps us to avoid cross bank
  /// copies. InstType deppends on context.
  enum InstType {
    /// Temporary type, when visit(..., nullptr) finishes will convert to one of
    /// the remaining types: Integer, FloatingPoint or Ambiguous.
    NotDetermined,
    /// Connected with instruction that interprets 'bags of bits' as integers.
    /// Select gprb to avoid cross bank copies.
    Integer,
    /// Connected with instruction that interprets 'bags of bits' as floating
    /// point numbers. Select fprb to avoid cross bank copies.
    FloatingPoint,
    /// Represents moving 'bags of bits' around. Select same bank for entire
    /// chain to avoid cross bank copies. Currently we select fprb for s64 and
    /// gprb for s32 Ambiguous operands.
    Ambiguous,
    /// Only used for s64. Unlike Ambiguous s64, AmbiguousWithMergeOrUnmerge s64
    /// is mapped to gprb (legalized using narrow scalar to s32).
    AmbiguousWithMergeOrUnmerge
  };

  bool isAmbiguous_64(InstType InstTy, unsigned OpSize) const {
    if (InstTy == InstType::Ambiguous && OpSize == 64)
      return true;
    return false;
  }

  bool isAmbiguous_32(InstType InstTy, unsigned OpSize) const {
    if (InstTy == InstType::Ambiguous && OpSize == 32)
      return true;
    return false;
  }

  bool isAmbiguous_32or64(InstType InstTy, unsigned OpSize) const {
    if (InstTy == InstType::Ambiguous && (OpSize == 32 || OpSize == 64))
      return true;
    return false;
  }

  bool isAmbiguousWithMergeOrUnmerge_64(InstType InstTy,
                                        unsigned OpSize) const {
    if (InstTy == InstType::AmbiguousWithMergeOrUnmerge && OpSize == 64)
      return true;
    return false;
  }

  bool isFloatingPoint_32or64(InstType InstTy, unsigned OpSize) const {
    if (InstTy == InstType::FloatingPoint && (OpSize == 32 || OpSize == 64))
      return true;
    return false;
  }

  bool isFloatingPoint_64(InstType InstTy, unsigned OpSize) const {
    if (InstTy == InstType::FloatingPoint && OpSize == 64)
      return true;
    return false;
  }

  bool isInteger_32(InstType InstTy, unsigned OpSize) const {
    if (InstTy == InstType::Integer && OpSize == 32)
      return true;
    return false;
  }

  /// Some generic instructions have operands that can be mapped to either fprb
  /// or gprb e.g. for G_LOAD we consider only operand 0 as ambiguous, operand 1
  /// is always gprb since it is a pointer.
  /// This class provides containers for MI's ambiguous:
  /// DefUses : MachineInstrs that use one of MI's ambiguous def operands.
  /// UseDefs : MachineInstrs that define MI's ambiguous use operands.
  class AmbiguousRegDefUseContainer {
    SmallVector<MachineInstr *, 2> DefUses;
    SmallVector<MachineInstr *, 2> UseDefs;

    void addDefUses(Register Reg, const MachineRegisterInfo &MRI);
    void addUseDef(Register Reg, const MachineRegisterInfo &MRI);

    /// Skip copy instructions until we get to a non-copy instruction or to a
    /// copy with phys register as def. Used during search for DefUses.
    /// MI :  %5 = COPY %4
    ///       %6 = COPY %5
    ///       $v0 = COPY %6 <- we want this one.
    MachineInstr *skipCopiesOutgoing(MachineInstr *MI) const;

    /// Skip copy instructions until we get to a non-copy instruction or to a
    /// copy with phys register as use. Used during search for UseDefs.
    ///       %1 = COPY $a1 <- we want this one.
    ///       %2 = COPY %1
    /// MI =  %3 = COPY %2
    MachineInstr *skipCopiesIncoming(MachineInstr *MI) const;

  public:
    AmbiguousRegDefUseContainer(const MachineInstr *MI);
    SmallVectorImpl<MachineInstr *> &getDefUses() { return DefUses; }
    SmallVectorImpl<MachineInstr *> &getUseDefs() { return UseDefs; }
  };

  class TypeInfoForMF {
    /// MachineFunction name is used to recognise when MF changes.
    std::string MFName = "";
    /// <key, value> : value is vector of all MachineInstrs that are waiting for
    /// key to figure out type of some of its ambiguous operands.
    DenseMap<const MachineInstr *, SmallVector<const MachineInstr *, 2>>
        WaitingQueues;
    /// Recorded InstTypes for visited instructions.
    DenseMap<const MachineInstr *, InstType> Types;

    /// Recursively visit MI's adjacent instructions and find MI's InstType.
    bool visit(const MachineInstr *MI, const MachineInstr *WaitingForTypeOfMI,
               InstType &AmbiguousTy);

    /// Visit MI's adjacent UseDefs or DefUses.
    bool visitAdjacentInstrs(const MachineInstr *MI,
                             SmallVectorImpl<MachineInstr *> &AdjacentInstrs,
                             bool isDefUse, InstType &AmbiguousTy);

    /// Set type for MI, and recursively for all instructions that are
    /// waiting for MI's type.
    void setTypes(const MachineInstr *MI, InstType ITy);

    /// InstType for MI is determined, set it to InstType that corresponds to
    /// physical regisiter that is operand number Op in CopyInst.
    void setTypesAccordingToPhysicalRegister(const MachineInstr *MI,
                                             const MachineInstr *CopyInst,
                                             unsigned Op);

    /// Set default values for MI in order to start visit.
    void startVisit(const MachineInstr *MI) {
      Types.try_emplace(MI, InstType::NotDetermined);
      WaitingQueues.try_emplace(MI);
    }

    /// Returns true if instruction was already visited. Type might not be
    /// determined at this point but will be when visit(..., nullptr) finishes.
    bool wasVisited(const MachineInstr *MI) const { return Types.count(MI); };

    /// Returns recorded type for instruction.
    const InstType &getRecordedTypeForInstr(const MachineInstr *MI) const {
      assert(wasVisited(MI) && "Instruction was not visited!");
      return Types.find(MI)->getSecond();
    };

    /// Change recorded type for instruction.
    void changeRecordedTypeForInstr(const MachineInstr *MI, InstType InstTy) {
      assert(wasVisited(MI) && "Instruction was not visited!");
      Types.find(MI)->getSecond() = InstTy;
    };

    /// Returns WaitingQueue for instruction.
    const SmallVectorImpl<const MachineInstr *> &
    getWaitingQueueFor(const MachineInstr *MI) const {
      assert(WaitingQueues.count(MI) && "Instruction was not visited!");
      return WaitingQueues.find(MI)->getSecond();
    };

    /// Add WaitingForMI to MI's WaitingQueue.
    void addToWaitingQueue(const MachineInstr *MI,
                           const MachineInstr *WaitingForMI) {
      assert(WaitingQueues.count(MI) && "Instruction was not visited!");
      WaitingQueues.find(MI)->getSecond().push_back(WaitingForMI);
    };

  public:
    InstType determineInstType(const MachineInstr *MI);

    void cleanupIfNewFunction(llvm::StringRef FunctionName);

    /// MI is about to get destroyed (using narrow scalar). Internal data is
    /// saved based on MI's address, clear it since it is no longer valid.
    void clearTypeInfoData(const MachineInstr *MI) {
      Types.erase(MI);
      WaitingQueues.erase(MI);
    };
  };
};
} // end namespace llvm
#endif
