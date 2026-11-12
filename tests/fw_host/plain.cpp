// Host test for the plain TeensyROM's VDrive (built with -DPLAIN_TR: no Fab04_* at all): firmware/IOH_VDrive.c without its KERNAL
// parts plus firmware/IOH_VDriveAgent.c, driven through the IO1 / IO2 handlers as the 6502 agent would (docs/DESIGN.md 53).
#include "stub.h"
#include <unistd.h>

FS SD, firstPartition;
bool g_sync_fail = false;
SerialStub Serial;
static uint8_t io1_mem[256];
volatile uint8_t *IO1 = io1_mem;
void (*fSpecialBtnChange)(bool Up_nDn) = nullptr;
char DriveDirPath[128];
EEPROMStub EEPROM;
uint8_t g_bus_in, g_bus_out;
bool g_led = true;
static uint32_t g_ms = 0;
uint32_t millis() { return g_ms; }
bool SDFullInit() { return fs_::exists(SD.root); }
bool USBFileSystemWait() { return fs_::exists(firstPartition.root); }
volatile uint8_t BtnPressed = 0;
bool (*fBusSnoop)(uint16_t Address, bool R_Wn) = nullptr;

#include "IO_Handlers/glue.c"   // = firmware/IOH_VDrive.c
#include "IO_Handlers/agent.c"  // = firmware/IOH_VDriveAgent.c

static int fails = 0;
static void check(const char *what, bool ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) fails++; }

static uint8_t rd1(uint8_t a) { g_bus_out = 0x77; IO1Hndlr_VDriveAgent(a, true); return g_bus_out; }
static void wr1(uint8_t a, uint8_t v) { g_bus_in = v; IO1Hndlr_VDriveAgent(a, false); }
static uint8_t rd2(uint8_t a) { g_bus_out = 0x77; IO2Hndlr_VDriveAgent(a, true); return g_bus_out; }
static void wr2(uint8_t a, uint8_t v) { g_bus_in = v; IO2Hndlr_VDriveAgent(a, false); }

int main(int argc, char **argv)
{
    if (argc < 2) { puts("usage: plain SDROOT"); return 2; }
    SD.root = argv[1]; firstPartition.root = "/nonexistent-usb";
    IO1[rWRegCurrMenuWAIT] = rmtSD;

    check("plain TR: the agent is the VDrive handler, named \"VDrive\"", strcmp(IOHndlr_VDriveAgent.Name, "VDrive") == 0
          && VD_IsHandler(IOH_VDriveAgent) && !VD_IsHandler(IOH_VDrive));
    check("plain TR: remap leaves every handler alone", VDriveSwift_Remap(IOH_Swiftlink) == IOH_Swiftlink && VDriveSwift_Remap(IOH_VDriveAgent) == IOH_VDriveAgent);
    check("PreStart is not the agent's for another handler", !VDrive_AgentPreStart(IOH_Swiftlink));
    check("PreStart for the agent: the drive's PreStart runs", VDrive_AgentPreStart(IOH_VDriveAgent) && vio && vioReady && vio->vd.unit[0].present);
    check("... and asks the menu for no bus probe (rwRegScratch stays 0)", IO1[rwRegScratch] == 0);
    InitHndlr_VDriveAgent();

    bool same = true;
    for (unsigned i = 0; i < sizeof VD_Agent; i++) same = same && rd1((uint8_t)(VD_AgentOfs + i)) == VD_Agent[i];
    check("IO1 $DE08..: the agent's bytes", same);
    check("IO1 $DE00-$DE07: open bus (a SwiftLink ACIA's place)", rd1(0x00) == 0x77 && rd1(0x07) == 0x77);
    wr1(0x80, 0x5C);
    check("IO1: the 6502's writes into the agent stay (its self-patched operands)", rd1(0x80) == 0x5C);
    InitHndlr_VDriveAgent();
    check("... and a new launch serves a fresh copy", rd1(0x80) == VD_Agent[0x80 - VD_AgentOfs]);

    check("IO2 $DF09/$DF0A: the ID the menu's loader checks before calling INSTALL", rd2(VD_AgentRegs + VIO_ID0) == VIO_ID0_VAL && rd2(VD_AgentRegs + VIO_ID1) == VIO_ID1_VAL);
    check("IO2 $DF0B-$DF7F: open bus", rd2(VD_AgentRegs + VIO_NREGS) == 0x77 && rd2(0x7F) == 0x77);
    check("IO2 $DF06: DEV0 (the SD unit, automatic: 8 until the first scan)", rd2(VD_AgentRegs + VIO_DEV0) == 8);

    // an INSTALL frame (entry 0) as the agent writes it: Y X A P, the return address low byte, ST, open files, DFLTN, DFLTO
    const uint8_t frame[] = { 0, 0, 0, 0x20, (uint8_t)(VD_AgentBase + 12 + 2), 0, 0, 0, 3 };
    for (unsigned i = 0; i < sizeof frame; i++) wr2((uint8_t)(VIO_WIN_OFS + i), frame[i]);
    wr2(VD_AgentRegs + VIO_CMD, VV_C_VEC);
    check("CMD: BUSY at once", rd2(VD_AgentRegs + VIO_STAT) == VIO_BUSY);
    PollingHndlr_VDriveAgent();
    check("after a poll: OK, and the answer starts with PEEK $0302 (INSTALL reads the vectors first)",
          rd2(VD_AgentRegs + VIO_STAT) == VIO_OK && rd2(VIO_WIN_OFS + 0) == VV_OP_PEEK && rd2(VIO_WIN_OFS + 1) == 0x02 && rd2(VIO_WIN_OFS + 2) == 0x03);

    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
