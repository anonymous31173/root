//===------ LeonPasses.cpp - Define passes specific to LEON ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//

#include "LeonPasses.h"
#include "llvm/CodeGen/ISDOpcodes.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

LEONMachineFunctionPass::LEONMachineFunctionPass(TargetMachine &tm, char &ID)
    : MachineFunctionPass(ID) {}

LEONMachineFunctionPass::LEONMachineFunctionPass(char &ID)
    : MachineFunctionPass(ID) {}

int LEONMachineFunctionPass::GetRegIndexForOperand(MachineInstr &MI,
                                                   int OperandIndex) {
  if (MI.getNumOperands() > 0) {
    if (OperandIndex == LAST_OPERAND) {
      OperandIndex = MI.getNumOperands() - 1;
    }

    if (MI.getNumOperands() > (unsigned)OperandIndex &&
        MI.getOperand(OperandIndex).isReg()) {
      return (int)MI.getOperand(OperandIndex).getReg();
    }
  }

  static int NotFoundIndex = -10;
  // Return a different number each time to avoid any comparisons between the
  // values returned.
  NotFoundIndex -= 10;
  return NotFoundIndex;
}

// finds a new free FP register
// checks also the AllocatedRegisters vector
int LEONMachineFunctionPass::getUnusedFPRegister(MachineRegisterInfo &MRI) {
  for (int RegisterIndex = SP::F0; RegisterIndex <= SP::F31; ++RegisterIndex) {
    if (!MRI.isPhysRegUsed(RegisterIndex) &&
        !(std::find(UsedRegisters.begin(), UsedRegisters.end(),
                    RegisterIndex) != UsedRegisters.end())) {
      return RegisterIndex;
    }
  }

  return -1;
}

//*****************************************************************************
//**** InsertNOPLoad pass
//*****************************************************************************
// This pass fixes the incorrectly working Load instructions that exists for
// some earlier versions of the LEON processor line. NOP instructions must
// be inserted after the load instruction to ensure that the Load instruction
// behaves as expected for these processors.
//
// This pass inserts a NOP after any LD or LDF instruction.
//
char InsertNOPLoad::ID = 0;

InsertNOPLoad::InsertNOPLoad(TargetMachine &tm)
    : LEONMachineFunctionPass(tm, ID) {}

bool InsertNOPLoad::runOnMachineFunction(MachineFunction &MF) {
  Subtarget = &MF.getSubtarget<SparcSubtarget>();
  const TargetInstrInfo &TII = *Subtarget->getInstrInfo();
  DebugLoc DL = DebugLoc();

  bool Modified = false;
  for (auto MFI = MF.begin(), E = MF.end(); MFI != E; ++MFI) {
    MachineBasicBlock &MBB = *MFI;
    for (auto MBBI = MBB.begin(), E = MBB.end(); MBBI != E; ++MBBI) {
      MachineInstr &MI = *MBBI;
      unsigned Opcode = MI.getOpcode();
      if (Opcode >= SP::LDDArr && Opcode <= SP::LDrr) {
        MachineBasicBlock::iterator NMBBI = std::next(MBBI);
        BuildMI(MBB, NMBBI, DL, TII.get(SP::NOP));
        Modified = true;
      } else if (MI.isInlineAsm()) {
        // Look for an inline ld or ldf instruction.
        StringRef AsmString =
            MI.getOperand(InlineAsm::MIOp_AsmString).getSymbolName();
        if (AsmString.startswith_lower("ld")) {
          MachineBasicBlock::iterator NMBBI = std::next(MBBI);
          BuildMI(MBB, NMBBI, DL, TII.get(SP::NOP));
          Modified = true;
        }
      }
    }
  }

  return Modified;
}

//*****************************************************************************
//**** FixFSMULD pass
//*****************************************************************************
// This pass fixes the incorrectly working FSMULD instruction that exists for
// some earlier versions of the LEON processor line.
//
// The pass should convert the FSMULD operands to double precision in scratch
// registers, then calculate the result with the FMULD instruction. Therefore,
// the pass should replace operations of the form:
// fsmuld %f20,%f21,%f8
// with the sequence:
// fstod %f20,%f0
// fstod %f21,%f2
// fmuld %f0,%f2,%f8
//
char FixFSMULD::ID = 0;

FixFSMULD::FixFSMULD(TargetMachine &tm) : LEONMachineFunctionPass(tm, ID) {}

bool FixFSMULD::runOnMachineFunction(MachineFunction &MF) {
  Subtarget = &MF.getSubtarget<SparcSubtarget>();
  const TargetInstrInfo &TII = *Subtarget->getInstrInfo();
  DebugLoc DL = DebugLoc();

  bool Modified = false;
  for (auto MFI = MF.begin(), E = MF.end(); MFI != E; ++MFI) {
    MachineBasicBlock &MBB = *MFI;
    for (auto MBBI = MBB.begin(), E = MBB.end(); MBBI != E; ++MBBI) {

      MachineInstr &MI = *MBBI;
      unsigned Opcode = MI.getOpcode();

      const int UNASSIGNED_INDEX = -1;
      int Reg1Index = UNASSIGNED_INDEX;
      int Reg2Index = UNASSIGNED_INDEX;
      int Reg3Index = UNASSIGNED_INDEX;

      if (Opcode == SP::FSMULD && MI.getNumOperands() == 3) {
        // take the registers from fsmuld %f20,%f21,%f8
        Reg1Index = MI.getOperand(0).getReg();
        Reg2Index = MI.getOperand(1).getReg();
        Reg3Index = MI.getOperand(2).getReg();
      } else if (MI.isInlineAsm()) {
        std::string AsmString(
            MI.getOperand(InlineAsm::MIOp_AsmString).getSymbolName());
        std::string FMULSOpCoode("fsmuld");
        std::transform(AsmString.begin(), AsmString.end(), AsmString.begin(),
                       ::tolower);
        if (AsmString.find(FMULSOpCoode) ==
            0) { // this is an inline FSMULD instruction

          unsigned StartOp = InlineAsm::MIOp_FirstOperand;

          // extracts the registers from the inline assembly instruction
          for (unsigned i = StartOp, e = MI.getNumOperands(); i != e; ++i) {
            const MachineOperand &MO = MI.getOperand(i);
            if (MO.isReg()) {
              if (Reg1Index == UNASSIGNED_INDEX)
                Reg1Index = MO.getReg();
              else if (Reg2Index == UNASSIGNED_INDEX)
                Reg2Index = MO.getReg();
              else if (Reg3Index == UNASSIGNED_INDEX)
                Reg3Index = MO.getReg();
            }
            if (Reg3Index != UNASSIGNED_INDEX)
              break;
          }
        }
      }

      if (Reg1Index != UNASSIGNED_INDEX && Reg2Index != UNASSIGNED_INDEX &&
          Reg3Index != UNASSIGNED_INDEX) {
        clearUsedRegisterList();
        MachineBasicBlock::iterator NMBBI = std::next(MBBI);
        // Whatever Reg3Index is hasn't been used yet, so we need to reserve it.
        markRegisterUsed(Reg3Index);
        const int ScratchReg1Index = getUnusedFPRegister(MF.getRegInfo());
        markRegisterUsed(ScratchReg1Index);
        const int ScratchReg2Index = getUnusedFPRegister(MF.getRegInfo());
        markRegisterUsed(ScratchReg2Index);

        if (ScratchReg1Index == UNASSIGNED_INDEX ||
            ScratchReg2Index == UNASSIGNED_INDEX) {
          errs() << "Cannot allocate free scratch registers for the FixFSMULD "
                    "pass."
                 << "\n";
        } else {
          // create fstod %f20,%f0
          BuildMI(MBB, MBBI, DL, TII.get(SP::FSTOD))
              .addReg(ScratchReg1Index)
              .addReg(Reg1Index);

          // create fstod %f21,%f2
          BuildMI(MBB, MBBI, DL, TII.get(SP::FSTOD))
              .addReg(ScratchReg2Index)
              .addReg(Reg2Index);

          // create fmuld %f0,%f2,%f8
          BuildMI(MBB, MBBI, DL, TII.get(SP::FMULD))
              .addReg(Reg3Index)
              .addReg(ScratchReg1Index)
              .addReg(ScratchReg2Index);

          MI.eraseFromParent();
          MBBI = NMBBI;

          Modified = true;
        }
      }
    }
  }

  return Modified;
}

//*****************************************************************************
//**** ReplaceFMULS pass
//*****************************************************************************
// This pass fixes the incorrectly working FMULS instruction that exists for
// some earlier versions of the LEON processor line.
//
// This pass converts the FMULS operands to double precision in scratch
// registers, then calculates the result with the FMULD instruction.
// The pass should replace operations of the form:
// fmuls %f20,%f21,%f8
// with the sequence:
// fstod %f20,%f0
// fstod %f21,%f2
// fmuld %f0,%f2,%f8
//
char ReplaceFMULS::ID = 0;

ReplaceFMULS::ReplaceFMULS(TargetMachine &tm)
    : LEONMachineFunctionPass(tm, ID) {}

bool ReplaceFMULS::runOnMachineFunction(MachineFunction &MF) {
  Subtarget = &MF.getSubtarget<SparcSubtarget>();
  const TargetInstrInfo &TII = *Subtarget->getInstrInfo();
  DebugLoc DL = DebugLoc();

  bool Modified = false;
  for (auto MFI = MF.begin(), E = MF.end(); MFI != E; ++MFI) {
    MachineBasicBlock &MBB = *MFI;
    for (auto MBBI = MBB.begin(), E = MBB.end(); MBBI != E; ++MBBI) {
      MachineInstr &MI = *MBBI;
      unsigned Opcode = MI.getOpcode();

      const int UNASSIGNED_INDEX = -1;
      int Reg1Index = UNASSIGNED_INDEX;
      int Reg2Index = UNASSIGNED_INDEX;
      int Reg3Index = UNASSIGNED_INDEX;

      if (Opcode == SP::FMULS && MI.getNumOperands() == 3) {
        // take the registers from fmuls %f20,%f21,%f8
        Reg1Index = MI.getOperand(0).getReg();
        Reg2Index = MI.getOperand(1).getReg();
        Reg3Index = MI.getOperand(2).getReg();
      } else if (MI.isInlineAsm()) {
        std::string AsmString(
            MI.getOperand(InlineAsm::MIOp_AsmString).getSymbolName());
        std::string FMULSOpCoode("fmuls");
        std::transform(AsmString.begin(), AsmString.end(), AsmString.begin(),
                       ::tolower);
        if (AsmString.find(FMULSOpCoode) ==
            0) { // this is an inline FMULS instruction
          unsigned StartOp = InlineAsm::MIOp_FirstOperand;

          // extracts the registers from the inline assembly instruction
          for (unsigned i = StartOp, e = MI.getNumOperands(); i != e; ++i) {
            const MachineOperand &MO = MI.getOperand(i);
            if (MO.isReg()) {
              if (Reg1Index == UNASSIGNED_INDEX)
                Reg1Index = MO.getReg();
              else if (Reg2Index == UNASSIGNED_INDEX)
                Reg2Index = MO.getReg();
              else if (Reg3Index == UNASSIGNED_INDEX)
                Reg3Index = MO.getReg();
            }
            if (Reg3Index != UNASSIGNED_INDEX)
              break;
          }
        }
      }

      if (Reg1Index != UNASSIGNED_INDEX && Reg2Index != UNASSIGNED_INDEX &&
          Reg3Index != UNASSIGNED_INDEX) {
        clearUsedRegisterList();
        MachineBasicBlock::iterator NMBBI = std::next(MBBI);
        // Whatever Reg3Index is hasn't been used yet, so we need to reserve it.
        markRegisterUsed(Reg3Index);
        const int ScratchReg1Index = getUnusedFPRegister(MF.getRegInfo());
        markRegisterUsed(ScratchReg1Index);
        const int ScratchReg2Index = getUnusedFPRegister(MF.getRegInfo());
        markRegisterUsed(ScratchReg2Index);

        if (ScratchReg1Index == UNASSIGNED_INDEX ||
            ScratchReg2Index == UNASSIGNED_INDEX) {
          errs() << "Cannot allocate free scratch registers for the "
                    "ReplaceFMULS pass."
                 << "\n";
        } else {
          // create fstod %f20,%f0
          BuildMI(MBB, MBBI, DL, TII.get(SP::FSTOD))
              .addReg(ScratchReg1Index)
              .addReg(Reg1Index);

          // create fstod %f21,%f2
          BuildMI(MBB, MBBI, DL, TII.get(SP::FSTOD))
              .addReg(ScratchReg2Index)
              .addReg(Reg2Index);

          // create fmuld %f0,%f2,%f8
          BuildMI(MBB, MBBI, DL, TII.get(SP::FMULD))
              .addReg(Reg3Index)
              .addReg(ScratchReg1Index)
              .addReg(ScratchReg2Index);

          MI.eraseFromParent();
          MBBI = NMBBI;

          Modified = true;
        }
      }
    }
  }

  return Modified;
}

//*****************************************************************************
//**** FixAllFDIVSQRT pass
//*****************************************************************************
// This pass fixes the incorrectly working FDIVx and FSQRTx instructions that
// exist for some earlier versions of the LEON processor line. Five NOP
// instructions need to be inserted after these instructions to ensure the
// correct result is placed in the destination registers before they are used.
//
// This pass implements two fixes:
//  1) fixing the FSQRTS and FSQRTD instructions.
//  2) fixing the FDIVS and FDIVD instructions.
//
// FSQRTS and FDIVS are converted to FDIVD and FSQRTD respectively earlier in
// the pipeline when this option is enabled, so this pass needs only to deal
// with the changes that still need implementing for the "double" versions
// of these instructions.
//
char FixAllFDIVSQRT::ID = 0;

FixAllFDIVSQRT::FixAllFDIVSQRT(TargetMachine &tm)
    : LEONMachineFunctionPass(tm, ID) {}

bool FixAllFDIVSQRT::runOnMachineFunction(MachineFunction &MF) {
  Subtarget = &MF.getSubtarget<SparcSubtarget>();
  const TargetInstrInfo &TII = *Subtarget->getInstrInfo();
  DebugLoc DL = DebugLoc();

  bool Modified = false;
  for (auto MFI = MF.begin(), E = MF.end(); MFI != E; ++MFI) {
    MachineBasicBlock &MBB = *MFI;
    for (auto MBBI = MBB.begin(), E = MBB.end(); MBBI != E; ++MBBI) {
      MachineInstr &MI = *MBBI;
      unsigned Opcode = MI.getOpcode();

      if (MI.isInlineAsm()) {
        std::string AsmString(
            MI.getOperand(InlineAsm::MIOp_AsmString).getSymbolName());
        std::string FSQRTDOpCode("fsqrtd");
        std::string FDIVDOpCode("fdivd");
        std::transform(AsmString.begin(), AsmString.end(), AsmString.begin(),
                       ::tolower);
        if (AsmString.find(FSQRTDOpCode) ==
            0) { // this is an inline fsqrts instruction
          Opcode = SP::FSQRTD;
        } else if (AsmString.find(FDIVDOpCode) ==
                   0) { // this is an inline fsqrts instruction
          Opcode = SP::FDIVD;
        }
      }

      // Note: FDIVS and FSQRTS cannot be generated when this erratum fix is
      // switched on so we don't need to check for them here. They will
      // already have been converted to FSQRTD or FDIVD earlier in the
      // pipeline.
      if (Opcode == SP::FSQRTD || Opcode == SP::FDIVD) {
        for (int InsertedCount = 0; InsertedCount < 5; InsertedCount++)
          BuildMI(MBB, MBBI, DL, TII.get(SP::NOP));

        MachineBasicBlock::iterator NMBBI = std::next(MBBI);
        for (int InsertedCount = 0; InsertedCount < 28; InsertedCount++)
          BuildMI(MBB, NMBBI, DL, TII.get(SP::NOP));

        Modified = true;
      }
    }
  }

  return Modified;
}
