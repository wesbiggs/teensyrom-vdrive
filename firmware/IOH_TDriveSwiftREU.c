// "TDrive+Swift+REU" Special IO for TeensyROM+: the virtual-drive KERNAL (IOH_TDrive.c), the SwiftLink / Turbo232 modem
// emulation (IOH_Swiftlink.c) and the REU (IOH_REU.c) at the same time.
//   IO1 $DE00-$DE0F -> SwiftLink ACIA, IO1 $DE10-$DE1A -> TDrive mailbox (as in IOH_TDriveSwift.c)
//   IO2 $DF00-$DF7F -> REU registers (11 registers, repeated every 32 bytes as on a real 1764/1750, but only 4 times)
//   IO2 $DF80-$DFFF -> TDrive 128-byte window
//   per-cycle callback -> SwiftLink (NMI timing); init and polling run all three.
// Memory: SwiftLink's receive queues take most of RAM2, so the REU gets the 128 KB RAM1 image (free: a PRG launch loads no
// cartridge) plus whatever RAM2 is left, rounded down to a power of two by InitHndlr_REU: 128 KB (a 1700) on hardware
// with 0.9.0.51 (REUTEST). Below 128 KB InitHndlr_REU disables it and the REU registers read as open bus; TDrive and SwiftLink still work.
// The IO2 split and the bus-snoop hand-back (VDREU_Snoop) are shared with "TDrive+REU" in IOH_TDriveREU.c.
// Included from IOHandlers.h after IOH_TDrive.c, IOH_REU.c, IOH_Swiftlink.c, IOH_TDriveSwift.c and IOH_TDriveREU.c (unity build,
// compiled as C++).

void InitHndlr_TDriveSwiftREU();
void PollingHndlr_TDriveSwiftREU();

stcIOHandlers IOHndlr_TDriveSwiftREU =
{
  "TDrive+Swift+REU",       //Name of handler (IOHNameLength max)
  &InitHndlr_TDriveSwiftREU,//Called once at handler startup
  &IO1Hndlr_TDriveSwift,    //IO1 R/W handler: ACIA below $10, mailbox registers above
  &IO2Hndlr_TDriveREU,      //IO2 R/W handler: REU registers below $80, TDrive window above (IOH_TDriveREU.c)
  NULL,                     //ROML Read handler, in addition to any ROM data sent
  NULL,                     //ROMH Read handler, in addition to any ROM data sent
  &PollingHndlr_TDriveSwiftREU,//Polled in main routine
  &CycleHndlr_SwiftLink,    //called at the end of EVERY c64 cycle
};

FLASHMEM void InitHndlr_TDriveSwiftREU()
{
   // Order matters. SwiftLink first: its fixed-size queues must fit, and the REU takes what RAM2 is left. REU before TDrive:
   // InitHndlr_REU clears PendingfBusSnoop and sets the alt button to "save REU"; InitHndlr_TDrive then re-stages KernalCheck
   // and, when the alt-button setting is "None", takes the button for disk switching.
   InitHndlr_SwiftLink();
   Serial.printf("TDrive: RAM2 free before the REU: %lu\n", RAM2BytesFree());
   InitHndlr_REU();
   vdReuBase = NULL;
   InitHndlr_TDrive();
}

FLASHMEM void PollingHndlr_TDriveSwiftREU()
{
   PollingHndlr_SwiftLink();
   PollingHndlr_REU();
   PollingHndlr_TDrive();
}
