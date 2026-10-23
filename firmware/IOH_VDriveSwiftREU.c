// "VDrive+Swift+REU" Special IO for TeensyROM+: the virtual-drive KERNAL (IOH_VDrive.c), the SwiftLink / Turbo232 modem
// emulation (IOH_Swiftlink.c) and the REU (IOH_REU.c) at the same time.
//   IO1 $DE00-$DE0F -> SwiftLink ACIA, IO1 $DE10-$DE18 -> VDrive mailbox (as in IOH_VDriveSwift.c)
//   IO2 $DF00-$DF7F -> REU registers (11 registers, repeated every 32 bytes as on a real 1764/1750, but only 4 times)
//   IO2 $DF80-$DFFF -> VDrive 128-byte window
//   per-cycle callback -> SwiftLink (NMI timing); init and polling run all three.
// Memory: SwiftLink's receive queues take most of RAM2, so the REU gets the 128 KB RAM1 image (free: a PRG launch loads no
// cartridge) plus whatever RAM2 is left, rounded down to a power of two by InitHndlr_REU: in practice a 128 KB (1700-size)
// REU. Below 128 KB InitHndlr_REU disables it and the REU registers read as open bus; VDrive and SwiftLink still work.
// Included from IOHandlers.h after IOH_VDrive.c, IOH_REU.c, IOH_Swiftlink.c and IOH_VDriveSwift.c (unity build, compiled as C++).

void InitHndlr_VDriveSwiftREU();
void IO2Hndlr_VDriveSwiftREU(uint8_t Address, bool R_Wn);
void PollingHndlr_VDriveSwiftREU();

stcIOHandlers IOHndlr_VDriveSwiftREU =
{
  "VDrive+Swift+REU",       //Name of handler (IOHNameLength max)
  &InitHndlr_VDriveSwiftREU,//Called once at handler startup
  &IO1Hndlr_VDriveSwift,    //IO1 R/W handler: ACIA below $10, mailbox registers above
  &IO2Hndlr_VDriveSwiftREU, //IO2 R/W handler: REU registers below $80, VDrive window above
  NULL,                     //ROML Read handler, in addition to any ROM data sent
  NULL,                     //ROMH Read handler, in addition to any ROM data sent
  &PollingHndlr_VDriveSwiftREU,//Polled in main routine
  &CycleHndlr_SwiftLink,    //called at the end of EVERY c64 cycle
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

FASTRUN void IO2Hndlr_VDriveSwiftREU(uint8_t Address, bool R_Wn)
{
   if (Address & VIO_WIN_OFS)
   {
      IO2Hndlr_VDrive(Address, R_Wn);
      return;
   }
   bool (*Prev)(uint16_t, bool) = fBusSnoop;
   IO2Hndlr_REU(Address, R_Wn);
   if (fBusSnoop == Prev) return;                     // the usual case: a register read or write that started nothing
   if (Prev != &VDREU_Snoop) vdReuBase = Prev;        // not armed before: remember what to go back to
   fBusSnoop = (fBusSnoop == &REU_FF00_W_Check) ? &VDREU_Snoop   // armed: watch for $FF00, keep serving the KERNAL meanwhile
                                                 : vdReuBase;    // cleared (immediate start / re-armed without $FF00)
}

FLASHMEM void InitHndlr_VDriveSwiftREU()
{
   // Order matters. SwiftLink first: its fixed-size queues must fit, and the REU takes what RAM2 is left. REU before VDrive:
   // InitHndlr_REU clears PendingfBusSnoop and sets the alt button to "save REU"; InitHndlr_VDrive then re-stages KernalCheck
   // and, when the alt-button setting is "None", takes the button for disk switching.
   InitHndlr_SwiftLink();
   InitHndlr_REU();
   vdReuBase = NULL;
   InitHndlr_VDrive();
}

FLASHMEM void PollingHndlr_VDriveSwiftREU()
{
   PollingHndlr_SwiftLink();
   PollingHndlr_REU();
   PollingHndlr_VDrive();
}
