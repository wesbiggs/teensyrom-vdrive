// Host test for firmware/IOH_VDrive.c: drives it exactly like the 6502 would, through the
// IO1/IO2 handlers (the ISR entry points) and the polling handler.
#include "stub.h"
#include <unistd.h>

FS SD, firstPartition;
SerialStub Serial;
EEPROMStub EEPROM;
uint8_t g_bus_in, g_bus_out;
static uint32_t g_ms = 0;
uint32_t millis() { return g_ms; }
static int g_fs_probes = 0;
bool SDFullInit() { g_fs_probes++; return fs_::exists(SD.root); }
bool USBFileSystemWait() { g_fs_probes++; return fs_::exists(firstPartition.root); }
void InitHndlr_KernalReplace() {}

#include "glue.c"          // = firmware/IOH_VDrive.c (with ../vdrive/*.c resolved by the build script)

static int fails = 0;
static void check(const char *what, bool ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) fails++; }

static void w1(uint8_t reg, uint8_t v) { g_bus_in = v; IO1Hndlr_VDrive(reg, false); }
static uint8_t r1(uint8_t reg) { IO1Hndlr_VDrive(reg, true); return g_bus_out; }
static void ww(uint8_t off, uint8_t v) { g_bus_in = v; IO2Hndlr_VDrive(off, false); }
static uint8_t wr_(uint8_t off) { IO2Hndlr_VDrive(off, true); return g_bus_out; }

// returns (notmine<<8)|data ; st in *st
static int call(uint8_t cmd, uint8_t arg = 0, uint8_t len = 0, uint8_t *st = nullptr)
{
    w1(VIO_ARG, arg); w1(VIO_LEN, len); w1(VIO_CMD, cmd);
    if (!(r1(VIO_STAT) & VIO_BUSY)) { puts("BUSY not visible"); fails++; }
    int guard = 0;
    while (r1(VIO_STAT) & VIO_BUSY) { PollingHndlr_VDrive(); if (++guard > 10) { puts("poll stuck"); fails++; break; } }
    if (st) *st = r1(VIO_ST);
    return ((r1(VIO_STAT) & VIO_NOTMINE) ? 0x100 : 0) | r1(VIO_DATA);
}

static void put_name(const std::string &n) { for (size_t i = 0; i < n.size(); i++) ww((uint8_t)i, (uint8_t)n[i]); }

static std::string fast_load(int dev, const std::string &name, int *rc = nullptr)
{
    put_name(name);
    int r = call(VIO_FLOAD_OPEN, dev, (uint8_t)name.size());
    if (rc) *rc = r;
    std::string out;
    if (r) return out;
    out.push_back((char)wr_(0)); out.push_back((char)wr_(1));
    for (;;) {
        uint8_t st; int n = call(VIO_FLOAD_BLK, 0, 0, &st) & 0xFF;
        for (int i = 0; i < n; i++) out.push_back((char)wr_((uint8_t)i));
        if (st & 0x40) break;
    }
    return out;
}

static int fast_save(int dev, const std::string &name, const std::string &data)
{
    put_name(name);
    int r = call(VIO_FSAVE_OPEN, dev, (uint8_t)name.size());
    if (r) return r;
    ww(0, 0x01); ww(1, 0x08); call(VIO_FSAVE_BLK, 0, 2);
    for (size_t i = 0; i < data.size(); i += 255) {
        size_t n = std::min<size_t>(255, data.size() - i);
        for (size_t j = 0; j < n; j++) ww((uint8_t)j, (uint8_t)data[i + j]);
        call(VIO_FSAVE_BLK, 0, (uint8_t)n);
    }
    return call(VIO_FSAVE_CLOSE) & 0xFF;
}

static void cmd15(const std::string &text)       // OPEN-less: LISTEN 8, SECOND $6F, bytes, UNLSN
{
    call(VD_LISTEN, 8); call(VD_SECOND, 0x6F);
    for (char c : text) call(VD_CIOUT, (uint8_t)c);
    call(VD_UNLSN);
}

static std::string status15()
{
    std::string s; uint8_t st;
    call(VD_TALK, 8); call(VD_TKSA, 0x6F);
    for (;;) { int d = call(VD_ACPTR, 0, 0, &st) & 0xFF; s.push_back((char)d); if (st & 0x40) break; }
    call(VD_UNTLK);
    return s;
}

int main(int argc, char **argv)
{
    if (argc < 4) { puts("usage: fwhost SD_ROOT USB_ROOT EXPECT_HELLO_FILE"); return 2; }
    SD.root = argv[1]; firstPartition.root = argv[2];
    std::string hello_expect;
    { FILE *f = fopen(argv[3], "rb"); char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) hello_expect.append(b, n); fclose(f); }

    memset(EEPROM.m, 0xFF, sizeof EEPROM.m);               // erased flash: no setting stored yet
    VDrive_PreStart();
    check("pre-start probed SD and USB", g_fs_probes == 2);
    InitHndlr_VDrive();
    check("handler start does no storage access", g_fs_probes == 2);
    check("init allocated the mailbox", vio != NULL);
    check("erased EEPROM -> default device numbers 8/9", vio->vd.dev[0] == 8 && vio->vd.dev[1] == 9);
    check("handler table entry named", strcmp(IOHndlr_VDrive.Name, "VDrive KERNAL") == 0 && IOHndlr_VDrive.IO1Hndlr && IOHndlr_VDrive.IO2Hndlr && IOHndlr_VDrive.PollingHndlr);

    // IO1 reads of unmapped registers must not drive the bus
    g_bus_out = 0x77; IO1Hndlr_VDrive(0x40, true);
    check("unmapped IO1 register left alone", g_bus_out == 0x77);

    // nothing mounted: LOAD"$" lists the SD card
    int rc; std::string d = fast_load(8, "$", &rc);
    check("host directory listing (SD, nothing mounted)", rc == 0 && d.find("\"GAME.D64\"") != std::string::npos
          && d.find("\"SUB\"") != std::string::npos && d.find("\"SD:/") != std::string::npos);
    check("dot files hidden", d.find(".HIDDEN") == std::string::npos);
    check("over-long names skipped", d.find("AVERYLONGFILENAME") == std::string::npos);

    // mount via CD: through the byte-level bus
    cmd15("CD:GAME.D64");
    check("CD:GAME.D64 ok", status15() == "00, OK,00,00\r");
    d = fast_load(8, "$");
    check("mounted image listing", d.find("GAMEDISK") != std::string::npos && d.find("\"HELLO\"") != std::string::npos);
    std::string h = fast_load(8, "HELLO", &rc);
    check("fast load from the mounted image matches", rc == 0 && h == hello_expect);

    // save, then let the idle flush run
    std::string payload(3000, 'Q');
    check("fast save ok", fast_save(8, "NEWFILE", payload) == 0);
    std::string back = fast_load(8, "NEWFILE", &rc);
    check("saved file reads back", rc == 0 && back.size() == payload.size() + 2 && back.substr(2) == payload);
    g_ms += 1000; PollingHndlr_VDrive();                       // > VD_FlushIdleMs since last write
    check("dirty flag flushed", !vdUnit[0].dirty);

    // unmount, directory navigation
    cmd15("CD:_");
    d = fast_load(8, "$");
    check("unmounted: back to host listing", d.find("\"SD:/") != std::string::npos);
    cmd15("CD:SUB");  check("CD into SUB", status15() == "00, OK,00,00\r");
    d = fast_load(8, "$");
    check("listing of SUB", d.find("\"SD:/SUB ") != std::string::npos && d.find("\"INNER.D64\"") != std::string::npos);
    cmd15("CD:INNER.D64"); check("mount from subdirectory", status15() == "00, OK,00,00\r");
    cmd15("CD:_"); cmd15("CD:.."); d = fast_load(8, "$");
    check("CD:.. returns to root", d.find("\"GAME.D64\"") != std::string::npos);
    cmd15("CD:NOPE.D64"); check("missing image -> 62", status15().rfind("62,FILE NOT FOUND", 0) == 0);
    check("a failed mount creates no file on the card", !fs_::exists(SD.root / "nope.d64") && !fs_::exists(SD.root / "NOPE.D64"));
    { FILE *f = fopen((SD.root / "TINY.D64").c_str(), "wb"); fputs("not an image", f); fclose(f); }
    cmd15("CD:TINY.D64"); check("undersized file -> 74 DRIVE NOT READY", status15().rfind("74,DRIVE NOT READY", 0) == 0);
    check("... and it is left untouched", fs_::file_size(SD.root / "TINY.D64") == 12);

    // USB unit (device 9) is independent
    int nm = call(VD_LISTEN, 9); call(VD_UNLSN);
    check("device 9 present", !(nm & 0x100));
    put_name("$"); int r9 = call(VIO_FLOAD_OPEN, 9, 1);
    check("device 9 lists the USB root", r9 == 0 && wr_(0) == 0x01 && wr_(1) == 0x04);
    while (!(call(VIO_FLOAD_BLK) , r1(VIO_ST) & 0x40)) {}
    call(VD_LISTEN, 9); call(VD_SECOND, 0x6F);
    for (char c : std::string("CD:USB.D64")) call(VD_CIOUT, (uint8_t)c);
    call(VD_UNLSN);
    d = fast_load(9, "$");
    check("device 9 mounted its own image", d.find("USBDISK") != std::string::npos && vdUnit[1].mounted && !vdUnit[0].mounted);

    // foreign device is not ours
    nm = call(VD_LISTEN, 4);
    check("device 4 -> not mine", (nm & 0x100) != 0);

    // ---- device numbers persisted in EEPROM, changeable from BASIC (U0>+CHR$(n)) ----
    cmd15(std::string("U0>") + char(10));                   // OPEN15,8,15,"U0>"+CHR$(10)
    check("U0> wrote unit 0 to EEPROM (10), unit 1 default (0)", EEPROM.m[eepAdVDriveDevs] == 10 && EEPROM.m[eepAdVDriveDevs + 1] == 0);
    nm = call(VD_LISTEN, 8);                                // 8 is now a real-bus device
    check("device 8 -> not mine after renumbering", (nm & 0x100) != 0);
    nm = call(VD_LISTEN, 10); call(VD_UNLSN);
    check("device 10 is the SD unit", !(nm & 0x100));
    VDrive_PreStart();                                     // 'power cycle'
    check("numbers survive a restart (10/9)", vio->vd.dev[0] == 10 && vio->vd.dev[1] == 9);
    EEPROM.m[eepAdVDriveDevs] = 7; EEPROM.m[eepAdVDriveDevs + 1] = 7;
    VDrive_PreStart();
    check("out-of-range/duplicate stored values fall back to defaults", vio->vd.dev[0] == 8 && vio->vd.dev[1] == 9);
    EEPROM.m[eepAdVDriveDevs] = 12; EEPROM.m[eepAdVDriveDevs + 1] = 12;
    VDrive_PreStart();
    check("duplicate stored values fall back to defaults", vio->vd.dev[0] == 8 && vio->vd.dev[1] == 9);

    printf("%s\n", fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
