// "VDrive" Special IO for the plain TeensyROM (and "VDrive Agent" on the TR+): the virtual drive without Kernal Replace
// (docs/DESIGN.md 52-53). The C64 keeps its own KERNAL; its RAM vectors point into a 246-byte 6502 agent (vector/vdagent.s,
// vd_agent.h) that this handler serves from IO1, and the agent reports every call to the vector engine (d64/vdrive_vec.c),
// which does OPEN..SAVE and the file table on top of the drive the KERNAL build uses. The drive itself, the SD / USB glue,
// PreStart, the remote control and the LED are IOH_VDrive.c's.
//   IO1 $DE08-$DEFF -> the agent (it patches its own operands, so writes are kept: the page lives in RAM1)
//   IO1 $DE00-$DE07 -> nothing (left for a SwiftLink ACIA)
//   IO2 $DF00-$DF0A -> the mailbox registers (as IO1 $DE10 in the KERNAL build), $DF80-$DFFF -> the 128-byte window
// The menu's PRG loader calls the agent's INSTALL entry once the program is in memory (PRGLoadStartReloc.s); after a reset
// SYS 56852 hooks the vectors again.
// A C128 in 128 mode (docs/DESIGN.md 56): a launch through upstream's C128Prg.ino runs any VDrive Special IO as this one (the MMU
// never maps a cartridge's KERNAL in there); the boot ROM's loader (C128PrgBoot.asm) calls INSTALL, and the engine tells the
// C128's KERNAL from a C64's there by itself.
// Included from IOHandlers.h after IOH_VDrive.c (unity build, compiled as C++).

#include "../vdrive/vdrive_vec.c"
#include "../vdrive/vd_agent.h"     // VD_Agent[], VD_AgentBase: vector/vdagent.s for this layout (tools/build_agent.py)

void InitHndlr_VDriveAgent();
void PollingHndlr_VDriveAgent();
void IO1Hndlr_VDriveAgent(uint8_t Address, bool R_Wn);
void IO2Hndlr_VDriveAgent(uint8_t Address, bool R_Wn);

stcIOHandlers IOHndlr_VDriveAgent =
{
#ifdef Fab04_VDrive
  "VDrive Agent",           //Name of handler (IOHNameLength max): the TR+ has the KERNAL one as "VDrive KERNAL"
#else
  "VDrive",                 //Name of handler (IOHNameLength max)
#endif
  &InitHndlr_VDriveAgent,   //Called once at handler startup
  &IO1Hndlr_VDriveAgent,    //IO1 R/W handler: the agent
  &IO2Hndlr_VDriveAgent,    //IO2 R/W handler: mailbox registers and the 128-byte window
  NULL,                     //ROML Read handler, in addition to any ROM data sent
  NULL,                     //ROMH Read handler, in addition to any ROM data sent
  &PollingHndlr_VDriveAgent,//Polled in main routine: the vector engine and all file system work
  NULL,                     //called at the end of EVERY c64 cycle
};

#ifndef Fab04_VDrive
// The plain TR's only VDrive handler is this one: a program the menu pairs with it (VDrive Config, IOH_VDriveMenu) needs no remap,
// and SwiftLink programs keep plain SwiftLink. (The TR+'s version, with the KERNAL's combined handlers, is in IOH_VDriveSwift.c.)
FLASHMEM uint8_t VDriveSwift_Remap(uint8_t ioh) { return ioh; }
#endif

// Called first in KERNALPreStart() (StatusFunctions.c), while the C64 waits: true when the next handler is this one, after the
// drive's PreStart (SD / USB probing, auto-mount, the remembered mounts). The KERNAL build's PreStart is then skipped.
FLASHMEM bool VDrive_AgentPreStart(uint8_t NextIOHndlr)
{
   if (VDriveSwift_Remap(NextIOHndlr) != IOH_VDriveAgent) return false;
   VDrive_PreStart(true);
   return true;
}

// Called from LaunchC128BasicPrg() (C128Prg.ino) once the launch into C128 mode is certain, while the menu still runs: with a
// VDrive Special IO as the default, this launch runs the agent (the KERNAL handlers serve a C64 KERNAL, which the MMU never maps
// in 128 mode; that also keeps their bus hook off the C128's own ROM), after the drive's PreStart, as KERNALPreStart() does for a
// C64 launch. Overrides the Special IO for this launch only, as upstream's own line above the call does for Kernal Replace:
// InitHndlr_TeensyROM() reads the default back from EEPROM on the next return to the menu.
// true: the agent runs this launch. Then an empty program (Exit to C128 BASIC) goes through the boot ROM too, not a plain reset,
// so its loader installs the agent; the loader leaves out the RUN for an empty program (C128Prg.ino, C128PrgBoot.asm).
FLASHMEM bool VDrive_C128Launch()
{
   if (!VD_IsHandler(VDriveSwift_Remap(IO1[rwRegNextIOHndlr]))) return false;
   IO1[rwRegNextIOHndlr] = IOH_VDriveAgent;
   VDrive_PreStart(true);
   Serial.println("VDrive: C128 mode, the agent");
   return true;
}

#define VD_AgentOfs (VD_AgentBase & 0xFF)
static_assert((VD_AgentBase >> 8) == 0xDE && VD_AgentOfs + sizeof VD_Agent <= 256, "the agent lives in IO1");

static uint8_t vdAgentPage[256];    // what IO1 answers: plain static = RAM1/DTCM, read on every agent fetch
static vvec_t vvecStore;            // the engine's contexts (~1.2 KB), main loop only; never DMAMEM (RAM2 statics shrink the REU's heap, DESIGN.md 50)
static vvec_t *vvec = NULL;

// The Alt button's border flash: none under the agent (it has no IRQ hook). 0.9.0.63-0.9.0.65 wrote $D020 by DMA here; those
// holds of a running program crashed it on hardware (docs/DESIGN.md 60), so the disk switches without a flash.

FLASHMEM void InitHndlr_VDriveAgent()
{
   memset(vdAgentPage, 0, sizeof vdAgentPage);
   memcpy(vdAgentPage + VD_AgentOfs, VD_Agent, sizeof VD_Agent);   // a fresh copy: the last run patched its operands
   if (vio == NULL) return;         // PreStart did not run (out of memory): nothing answers, the agent's vectors are never set
   vvec = &vvecStore;
   vvec_init(vvec, vio, VD_AgentBase);
#ifdef Fab04_VDrive
   VD_AltButtonInit();              // the Alt button switches disks under the agent too (TR+), without the border flash
#endif
   vdDrivePoll = &PollingHndlr_VDriveAgent;
}

FLASHMEM void PollingHndlr_VDriveAgent()
{
   if (vvec == NULL) return;
   VD_PollCommon(vvec_poll(vvec) != 0);
}

// The agent's bytes: every opcode the 6502 runs in here is one of these reads, so nothing may come before the answer.
FASTRUN void IO1Hndlr_VDriveAgent(uint8_t Address, bool R_Wn)
{
   if (Address < VD_AgentOfs) return;                      // $DE00-$DE07: open bus
   if (R_Wn) DataPortWriteWaitLog(vdAgentPage[Address]);
   else
   {
      uint8_t Data = DataPortWaitRead();
      TraceLogAddValidData(Data);
      vdAgentPage[Address] = Data;                         // the agent's self-patched operands (POKE, PEEK, CALL)
   }
}

FASTRUN void IO2Hndlr_VDriveAgent(uint8_t Address, bool R_Wn)
{
   if (Address >= VIO_WIN_OFS) { IO2Hndlr_VDrive(Address, R_Wn); return; }   // the window, as in the KERNAL build
   Address -= VD_AgentRegs;
   if (!vioReady || Address >= VIO_NREGS) return;          // not mapped: open bus
   if (R_Wn) DataPortWriteWaitLog(reinterpret_cast<volatile uint8_t *>(&vioHot)[Address]);
   else
   {
      uint8_t Data = DataPortWaitRead();
      TraceLogAddValidData(Data);
      vio_io1_write(&vioHot, Address, Data);
   }
}
