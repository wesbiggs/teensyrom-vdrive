// "TDrive+SwiftLink" Special IO for TeensyROM+: the virtual-drive KERNAL (IOH_TDrive.c) together with the SwiftLink / Turbo232
// modem emulation (IOH_Swiftlink.c), so a terminal such as CCGMS can download to device 8/9 through the normal KERNAL.
//   IO1 $DE00-$DE0F -> SwiftLink ACIA ($DE00-$DE03, Turbo232 speed register $DE07)
//   IO1 $DE10-$DE1A -> TDrive mailbox, IO2 $DF80-$DFFF -> TDrive 128-byte window
//   per-cycle callback -> SwiftLink (NMI timing); init and polling run both.
// Included from IOHandlers.h after IOH_TDrive.c and IOH_Swiftlink.c (unity build, compiled as C++).

void InitHndlr_TDriveSwift();
void IO1Hndlr_TDriveSwift(uint8_t Address, bool R_Wn);
void PollingHndlr_TDriveSwift();

stcIOHandlers IOHndlr_TDriveSwift =
{
  "TDrive+SwiftLink",       //Name of handler (IOHNameLength max)
  &InitHndlr_TDriveSwift,   //Called once at handler startup
  &IO1Hndlr_TDriveSwift,    //IO1 R/W handler: ACIA below $10, mailbox registers above
  &IO2Hndlr_TDrive,         //IO2 R/W handler: 128-byte data window at $DF80
  NULL,                     //ROML Read handler, in addition to any ROM data sent
  NULL,                     //ROMH Read handler, in addition to any ROM data sent
  &PollingHndlr_TDriveSwift,//Polled in main routine
  &CycleHndlr_SwiftLink,    //called at the end of EVERY c64 cycle
};

// When the user's default Special IO is a TDrive one, programs that the menu pairs with plain SwiftLink (CCGMS, ...) or the REU
// get a combined handler instead, so launching them does not drop the virtual drive: SwiftLink programs TDrive+SwiftLink, REU
// programs TDrive+REU (the full 512 KB REU, IOH_TDriveREU.c). With "TDrive+Swift+REU" as the default, SwiftLink programs get
// that one (its REU is only 128 KB, but they may want both). A program the menu pairs with plain TDrive (TDrive Config, in the
// Utilities) runs with the default TDrive Special IO itself.
FLASHMEM uint8_t TDriveSwift_Remap(uint8_t ioh)
{
   uint8_t pref = EEPROM.read(eepAdNextIOHndlr);
   if (!VD_IsHandler(pref)) return ioh;
   if (ioh == IOH_TDrive) return pref;                       // a TDrive program (TDrive Config) keeps the default's REU / SwiftLink
   if (pref == IOH_TDriveAgent) return ioh;                  // the agent has no combined handlers (yet): REU / SwiftLink programs get theirs
#ifdef Fab04_REU
   if (ioh == IOH_REU) return IOH_TDriveREU;
   if (pref == IOH_TDriveSwiftREU) return (ioh == IOH_Swiftlink) ? IOH_TDriveSwiftREU : ioh;
#endif
   return (ioh == IOH_Swiftlink) ? IOH_TDriveSwift : ioh;
}

FLASHMEM void InitHndlr_TDriveSwift()
{
   InitHndlr_SwiftLink();     // allocates the receive queues and frees the drive menu, as for plain SwiftLink
   InitHndlr_TDrive();        // re-stages the KERNAL bus hook
}

FASTRUN void IO1Hndlr_TDriveSwift(uint8_t Address, bool R_Wn)
{
   if (Address < VD_MboxBase) IO1Hndlr_SwiftLink(Address, R_Wn);
   else IO1Hndlr_TDrive(Address, R_Wn);
}

FLASHMEM void PollingHndlr_TDriveSwift()
{
   PollingHndlr_SwiftLink();
   PollingHndlr_TDrive();
}
