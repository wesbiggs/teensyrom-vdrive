// "VDrive+SwiftLink" Special IO for TeensyROM+: the virtual-drive KERNAL (IOH_VDrive.c) together with the SwiftLink / Turbo232
// modem emulation (IOH_Swiftlink.c), so a terminal such as CCGMS can download to device 8/9 through the normal KERNAL.
//   IO1 $DE00-$DE0F -> SwiftLink ACIA ($DE00-$DE03, Turbo232 speed register $DE07)
//   IO1 $DE10-$DE15 -> VDrive mailbox, IO2 -> VDrive 256-byte window
//   per-cycle callback -> SwiftLink (NMI timing); init and polling run both.
// Included from IOHandlers.h after IOH_VDrive.c and IOH_Swiftlink.c (unity build, compiled as C++).

void InitHndlr_VDriveSwift();
void IO1Hndlr_VDriveSwift(uint8_t Address, bool R_Wn);
void PollingHndlr_VDriveSwift();

stcIOHandlers IOHndlr_VDriveSwift =
{
  "VDrive+SwiftLink",       //Name of handler (IOHNameLength max)
  &InitHndlr_VDriveSwift,   //Called once at handler startup
  &IO1Hndlr_VDriveSwift,    //IO1 R/W handler: ACIA below $10, mailbox registers above
  &IO2Hndlr_VDrive,         //IO2 R/W handler: 256-byte data window
  NULL,                     //ROML Read handler, in addition to any ROM data sent
  NULL,                     //ROMH Read handler, in addition to any ROM data sent
  &PollingHndlr_VDriveSwift,//Polled in main routine
  &CycleHndlr_SwiftLink,    //called at the end of EVERY c64 cycle
};

// When the user's default Special IO is a VDrive one, programs that the menu pairs with plain SwiftLink (CCGMS, ...) get the
// combined handler instead, so launching them does not drop the virtual drive.
FLASHMEM uint8_t VDriveSwift_Remap(uint8_t ioh)
{
   if (ioh != IOH_Swiftlink) return ioh;
   uint8_t pref = EEPROM.read(eepAdNextIOHndlr);
   return (pref == IOH_VDrive || pref == IOH_VDriveSwift) ? IOH_VDriveSwift : ioh;
}

FLASHMEM void InitHndlr_VDriveSwift()
{
   InitHndlr_SwiftLink();     // allocates the receive queues and frees the drive menu, as for plain SwiftLink
   InitHndlr_VDrive();        // re-stages the KERNAL bus hook
}

FASTRUN void IO1Hndlr_VDriveSwift(uint8_t Address, bool R_Wn)
{
   if (Address < VD_MboxBase) IO1Hndlr_SwiftLink(Address, R_Wn);
   else IO1Hndlr_VDrive(Address, R_Wn);
}

FLASHMEM void PollingHndlr_VDriveSwift()
{
   PollingHndlr_SwiftLink();
   PollingHndlr_VDrive();
}
