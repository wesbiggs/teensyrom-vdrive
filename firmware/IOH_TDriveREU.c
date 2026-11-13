// "TDrive+REU" Special IO for TeensyROM+: the virtual-drive KERNAL (IOH_TDrive.c) and the REU (IOH_REU.c) at the same time.
//   IO1 $DE10-$DE1A -> TDrive mailbox ($DE00-$DE0F unanswered)
//   IO2 $DF00-$DF7F -> REU registers (11 registers, repeated every 32 bytes as on a real 1764/1750, but only 4 times)
//   IO2 $DF80-$DFFF -> TDrive 128-byte window
//   no per-cycle callback; init and polling run both.
// Memory: without SwiftLink's receive queues the REU gets the 128 KB RAM1 image plus 384 KB of RAM2: the full 512 KB of the
// plain REU handler (MAX_REU_BANKS). The TDrive state is plain static (DTCM), never DMAMEM: RAM2 statics shrink the heap the REU
// takes its banks from (before 0.9.0.53 ~9.4 KB of them left every REU, the plain one too, at 256 KB).
// Also holds the IO2 split and the bus-snoop hand-back that IOH_TDriveSwiftREU.c uses.
// Included from IOHandlers.h after IOH_TDrive.c and IOH_REU.c (unity build, compiled as C++).

void InitHndlr_TDriveREU();
uint32_t RAM2BytesFree();   // SerUSBIO.ino
void IO2Hndlr_TDriveREU(uint8_t Address, bool R_Wn);
void PollingHndlr_TDriveREU();

stcIOHandlers IOHndlr_TDriveREU =
{
  "TDrive+REU",             //Name of handler (IOHNameLength max)
  &InitHndlr_TDriveREU,     //Called once at handler startup
  &IO1Hndlr_TDrive,         //IO1 R/W handler: mailbox registers at $DE10
  &IO2Hndlr_TDriveREU,      //IO2 R/W handler: REU registers below $80, TDrive window above
  NULL,                     //ROML Read handler, in addition to any ROM data sent
  NULL,                     //ROMH Read handler, in addition to any ROM data sent
  &PollingHndlr_TDriveREU,  //Polled in main routine
  NULL,                     //called at the end of EVERY c64 cycle
};

// fBusSnoop is one pointer, and both sides want it: KernalCheck serves the replacement KERNAL on every cycle, and an REU command
// that waits for a write to $FF00 points it at REU_FF00_W_Check, which sets it to NULL when the transfer starts. Either would
// stop the KERNAL being served. So after every REU register access the hook is put back: while a $FF00 trigger is armed,
// VDREU_Snoop watches for it and otherwise runs the snoop that was there before (KernalCheck); after the trigger, or once an
// immediate command has cleared the hook, that snoop is back on its own.
static bool (*vdReuBase)(uint16_t Address, bool R_Wn) = NULL;   // the snoop the REU displaced (KernalCheck)

FASTRUN bool VDREU_Snoop(uint16_t Address, bool R_Wn)
{
   if (Address == 0xff00 && !R_Wn)
   {
      bool Skip = REU_FF00_W_Check(Address, R_Wn);   // starts the transfer (sets fBusSnoop = NULL)
      fBusSnoop = vdReuBase;
      return Skip;
   }
   return vdReuBase != NULL && vdReuBase(Address, R_Wn);
}

FASTRUN void IO2Hndlr_TDriveREU(uint8_t Address, bool R_Wn)
{
   if (Address & VIO_WIN_OFS)
   {
      IO2Hndlr_TDrive(Address, R_Wn);
      return;
   }
   bool (*Prev)(uint16_t, bool) = fBusSnoop;
   IO2Hndlr_REU(Address, R_Wn);
   if (fBusSnoop == Prev) return;                     // the usual case: a register read or write that started nothing
   if (Prev != &VDREU_Snoop) vdReuBase = Prev;        // not armed before: remember what to go back to
   fBusSnoop = (fBusSnoop == &REU_FF00_W_Check) ? &VDREU_Snoop   // armed: watch for $FF00, keep serving the KERNAL meanwhile
                                                 : vdReuBase;    // cleared (immediate start / re-armed without $FF00)
}

FLASHMEM void InitHndlr_TDriveREU()
{
   // REU before TDrive: InitHndlr_REU clears PendingfBusSnoop and sets the alt button to "save REU"; InitHndlr_TDrive then
   // re-stages KernalCheck and, when the alt-button setting is "None", takes the button for disk switching.
   Serial.printf("TDrive: RAM2 free before the REU: %lu\n", RAM2BytesFree());   // the REU needs 48 x 8 KB of it for 512 KB (docs/DESIGN.md 50)
   InitHndlr_REU();
   vdReuBase = NULL;
   InitHndlr_TDrive();
}

FLASHMEM void PollingHndlr_TDriveREU()
{
   PollingHndlr_REU();
   PollingHndlr_TDrive();
}
