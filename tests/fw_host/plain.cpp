// Host test for the plain TeensyROM's VDrive (built with -DPLAIN_TR: no Fab04_* at all): firmware/IOH_VDrive.c without its KERNAL
// parts plus firmware/IOH_VDriveAgent.c, driven through the IO1 / IO2 handlers as the 6502 agent would (docs/DESIGN.md 51).
#include "stub.h"
#include <unistd.h>

FS SD, firstPartition;
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
    check("IO2 $DF0C-$DF0F: open bus (between the registers and the hook block)", rd2(VD_AgentRegs + VIO_NREGS) == 0x77 && rd2(VD_HooksOfs - 1) == 0x77);
    same = true;
    for (unsigned i = 0; i < sizeof VD_Hooks; i++) same = same && rd2((uint8_t)(VD_HooksOfs + i)) == VD_Hooks[i];
    check("IO2 $DF10..: the hook block's bytes (vector/vdhooks.s)", same && VD_HooksBase == 0xDF10);
    check("IO2 after the hook block, up to $DF7F: zero (no code there)", rd2((uint8_t)(VD_HooksOfs + sizeof VD_Hooks)) == 0 && rd2(0x7F) == 0);
    wr2(VD_HooksOfs + 6, 0x05);
    check("IO2: the 6502's writes into the hook block stay (the saved border colour)", rd2(VD_HooksOfs + 6) == 0x05);
    InitHndlr_VDriveAgent();
    check("... and a new launch serves a fresh copy", rd2(VD_HooksOfs + 6) == VD_Hooks[6]);
    check("IO2 $DF06: DEV0 (the SD unit, automatic: 8 until the first scan)", rd2(VD_AgentRegs + VIO_DEV0) == 8);

    // an INSTALL frame (entry 0) as the agent writes it: Y X A P, the return address low byte, ST, open files, DFLTN, DFLTO
    const uint8_t frame[] = { 0, 0, 0, 0x20, (uint8_t)(VD_AgentBase + 12 + 2), 0, 0, 0, 3 };
    for (unsigned i = 0; i < sizeof frame; i++) wr2((uint8_t)(VIO_WIN_OFS + i), frame[i]);
    wr2(VD_AgentRegs + VIO_CMD, VV_C_VEC);
    check("CMD: BUSY at once", rd2(VD_AgentRegs + VIO_STAT) == VIO_BUSY);
    PollingHndlr_VDriveAgent();
    check("after a poll: OK, and the answer starts with PEEK $028F (INSTALL reads KEYLOG first: the hook block is served)",
          rd2(VD_AgentRegs + VIO_STAT) == VIO_OK && rd2(VIO_WIN_OFS + 0) == VV_OP_PEEK && rd2(VIO_WIN_OFS + 1) == 0x8F && rd2(VIO_WIN_OFS + 2) == 0x02);

    // the KEY register: the hook block's CTRL+C=+F1 / F3 (docs/DESIGN.md 53), the Alt button's switch and border flash
    {
        FILE *f = fopen((SD.root / "vdrive-config.txt").c_str(), "wb"); fputs("/GAME.D64,3,1\n/TWO.D71,6,1\n", f); fclose(f);
        auto cur = [&]() { char js[300]; if (!VDrive_RemoteStatus(0, js, sizeof js)) return std::string("?"); std::string j = js; size_t a = j.find("\"path\":\"");
                           std::string r = j.substr(a + 8, j.find('"', a + 8) - a - 8); for (auto &c : r) c = (char)tolower((unsigned char)c); return r; };
        auto run = [&](uint32_t ms) { std::vector<uint8_t> t; uint8_t last = vioHot.border;
                                      for (uint32_t i = 0; i < ms; i += 5) { g_ms += 5; PollingHndlr_VDriveAgent();
                                                                             if (vioHot.border != last) { last = vioHot.border; t.push_back(last); } }
                                      return t; };
        g_ms += 5000;
        std::string before = cur();
        wr2(VD_AgentRegs + VIO_KEY, 0x0A);                  // CTRL+C=+another key: ignored
        check("KEY: reads back what was written until the main loop takes it", rd2(VD_AgentRegs + VIO_KEY) == 0x0A);
        auto t = run(500);
        check("KEY: another key does nothing (and the register is cleared)", t.empty() && cur() == before && rd2(VD_AgentRegs + VIO_KEY) == 0);
        std::string first = before == "/game.d64" ? "/two.d71" : "/game.d64";
        uint8_t col = before == "/game.d64" ? 6 : 3;
        wr2(VD_AgentRegs + VIO_KEY, VIO_KEY_SD);            // CTRL+C=+F1
        t = run(2000);
        check("KEY F1: the SD unit's next image, at once (no double-tap wait)", cur() == first);
        check("KEY F1: its border flash (save, set colour, restore, idle)",
              t.size() == 4 && t[0] == VIO_BRD_SAVE && t[1] == (VIO_BRD_SET | col) && t[2] == VIO_BRD_RESTORE && t[3] == VIO_BRD_IDLE);
        wr2(VD_AgentRegs + VIO_KEY, VIO_KEY_SD);
        t = run(2000);
        check("KEY F1 again: the next one", cur() != first && (cur() == "/game.d64" || cur() == "/two.d71"));
        wr2(VD_AgentRegs + VIO_KEY, VIO_KEY_USB);           // CTRL+C=+F3: no USB stick here
        t = run(3000);
        check("KEY F3 with no USB stick: the red error flash", t.size() >= 2 && t[0] == VIO_BRD_SAVE && t[1] == (VIO_BRD_SET | VD_ColorRed));
        remove((SD.root / "vdrive-config.txt").c_str());
    }

    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
