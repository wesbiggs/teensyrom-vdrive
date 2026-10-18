#include <fstream>
// Host test for firmware/IOH_VDrive.c: drives it exactly like the 6502 would, through the
// IO1/IO2 handlers (the ISR entry points) and the polling handler.
#include "stub.h"
#include <unistd.h>
#include <set>

FS SD, firstPartition;
SerialStub Serial;
static uint8_t io1_mem[16];
volatile uint8_t *IO1 = io1_mem;
char DriveDirPath[128];
EEPROMStub EEPROM;
uint8_t g_bus_in, g_bus_out;
static uint32_t g_ms = 0;
uint32_t millis() { return g_ms; }
static int g_fs_probes = 0;
bool SDFullInit() { g_fs_probes++; return fs_::exists(SD.root); }
bool USBFileSystemWait() { g_fs_probes++; return fs_::exists(firstPartition.root); }
void InitHndlr_KernalReplace() {}

#include "glue.c"          // = firmware/IOH_VDrive.c (with ../vdrive/*.c resolved by the build script)

// stand-ins for IOH_Swiftlink.c: record which entry points the combined handler reaches
static int g_sw_io1 = 0, g_sw_init = 0, g_sw_poll = 0, g_sw_cycle = 0; static uint8_t g_sw_last = 0xEE;
void InitHndlr_SwiftLink() { g_sw_init++; }
void IO1Hndlr_SwiftLink(uint8_t a, bool) { g_sw_io1++; g_sw_last = a; }
void PollingHndlr_SwiftLink() { g_sw_poll++; }
void CycleHndlr_SwiftLink(bool) { g_sw_cycle++; }
#include "IO_Handlers/swift.c"   // = firmware/IOH_VDriveSwift.c

static int fails = 0;
static void check(const char *what, bool ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) fails++; }

static void w1(uint8_t reg, uint8_t v) { g_bus_in = v; IO1Hndlr_VDrive(VD_MboxBase + reg, false); }     // reg = mailbox offset
static uint8_t r1(uint8_t reg) { IO1Hndlr_VDrive(VD_MboxBase + reg, true); return g_bus_out; }
static void ww(uint8_t off, uint8_t v) { g_bus_in = v; IO2Hndlr_VDrive(off, false); }
static uint8_t wr_(uint8_t off) { IO2Hndlr_VDrive(off, true); return g_bus_out; }

static int g_scans = 0;                                    // bus scans the fake KERNAL did
static std::set<int> g_real_drives;                        // device numbers with a "real drive" on the fake bus

static void raw_call(uint8_t cmd, uint8_t arg, uint8_t len)
{
    w1(VIO_ARG, arg); w1(VIO_LEN, len); w1(VIO_CMD, cmd);
    if (r1(VIO_STAT) != VIO_BUSY) { puts("BUSY not visible"); fails++; }
    int guard = 0;
    while (r1(VIO_STAT) == VIO_BUSY) { PollingHndlr_VDrive(); if (++guard > 10) { puts("poll stuck"); fails++; break; } }
}

// returns (notmine<<8)|data ; st in *st. Like the patched KERNAL: a SCANREQ answer is followed by the bus scan, then the command again.
static int call(uint8_t cmd, uint8_t arg = 0, uint8_t len = 0, uint8_t *st = nullptr)
{
    raw_call(cmd, arg, len);
    if (r1(VIO_STAT) == VIO_SCANREQ) {
        g_scans++;
        raw_call(VIO_SCAN, 0, 0);
        int need = r1(VIO_DATA);
        for (int dev = 8; need > 0 && dev <= 30; dev++)
            if (!g_real_drives.count(dev)) { raw_call(VIO_FREE, (uint8_t)dev, 0); need = r1(VIO_DATA); }
        raw_call(cmd, arg, len);
    }
    if (st) *st = r1(VIO_ST);
    return ((r1(VIO_STAT) == VIO_NOTMINE) ? 0x100 : 0) | r1(VIO_DATA);
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

    // the mailbox lives at $DE10; the ACIA range below it is not ours
    g_bus_out = 0x77; IO1Hndlr_VDrive(0x00, true); IO1Hndlr_VDrive(0x03, true); IO1Hndlr_VDrive(0x07, true); IO1Hndlr_VDrive(0x08, true);
    IO1Hndlr_VDrive(VD_MboxBase + 8, true);
    check("mailbox is not mapped at $DE00-$DE07 or past $DE17", g_bus_out == 0x77);
    g_bus_in = 0x42; IO1Hndlr_VDrive(0x00, false);
    check("a write to $DE00 does not touch the mailbox", vioHot.arg != 0x42 && !vioHot.pending);

    // combined VDrive+SwiftLink handler
    check("combined entry named and fully wired", strcmp(IOHndlr_VDriveSwift.Name, "VDrive+SwiftLink") == 0 && strlen(IOHndlr_VDriveSwift.Name) < IOHNameLength
          && IOHndlr_VDriveSwift.IO2Hndlr == IO2Hndlr_VDrive && IOHndlr_VDriveSwift.CycleHndlr == CycleHndlr_SwiftLink);
    for (int a : {0x00, 0x01, 0x02, 0x03, 0x07, 0x0F}) { g_sw_last = 0xEE; IOHndlr_VDriveSwift.IO1Hndlr(a, true); check("ACIA offset goes to SwiftLink", g_sw_last == a); }
    g_sw_io1 = 0; g_bus_in = 0x31; IOHndlr_VDriveSwift.IO1Hndlr(VD_MboxBase + VIO_ARG, false);
    check("mailbox offset goes to VDrive, not SwiftLink", g_sw_io1 == 0 && vioHot.arg == 0x31);
    IOHndlr_VDriveSwift.InitHndlr(); IOHndlr_VDriveSwift.PollingHndlr();
    check("init and polling run both handlers", g_sw_init == 1 && g_sw_poll == 1);
    EEPROM.write(eepAdNextIOHndlr, IOH_None);
    check("remap: default Special IO not VDrive -> SwiftLink program stays plain", VDriveSwift_Remap(IOH_Swiftlink) == IOH_Swiftlink);
    EEPROM.write(eepAdNextIOHndlr, IOH_VDrive);
    check("remap: default VDrive -> SwiftLink program gets the combined handler", VDriveSwift_Remap(IOH_Swiftlink) == IOH_VDriveSwift);
    EEPROM.write(eepAdNextIOHndlr, IOH_VDriveSwift);
    check("remap: default VDrive+SwiftLink too", VDriveSwift_Remap(IOH_Swiftlink) == IOH_VDriveSwift);
    check("remap: other handlers pass through", VDriveSwift_Remap(IOH_Other) == IOH_Other && VDriveSwift_Remap(IOH_None) == IOH_None);
    EEPROM.write(eepAdNextIOHndlr, 0xFF);
    memset(EEPROM.m + eepAdVDriveDevs, 0xFF, 2);

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
    cmd15("CD:/"); check("CD:/ unmounts and returns to the root", status15() == "00, OK,00,00\r" && !vdUnit[0].mounted && vdUnit[0].cwd[0] == 0);
    cmd15("CD:SUB"); cmd15("CD:INNER.D64"); status15();
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
    check("duplicate stored values: the first is kept, the second becomes automatic", vio->vd.dev[0] == 12 && vio->vd.dev[1] == 9);

    // ---- image opened in the TR menu is mounted at launch (DriveDirPath ends in '*') ----
    EEPROM.m[eepAdVDriveDevs] = 0xFF; EEPROM.m[eepAdVDriveDevs + 1] = 0xFF;
    fs_::remove(SD.root / ".vdrive"); fs_::remove(firstPartition.root / ".vdrive");     // forget everything remembered so far
    IO1[rWRegCurrMenuWAIT] = rmtSD; strcpy(DriveDirPath, "/SUB/INNER.D64*");
    VDrive_PreStart();
    check("menu image on SD auto-mounted on unit 0 in its folder", vdUnit[0].mounted && !vdUnit[1].mounted && strcmp(vdUnit[0].cwd, "/SUB") == 0 && vio->vd.unit[0].disk);
    check("auto-mounted image is listed", fast_load(8, "$").find("\"") != std::string::npos);
    cmd15("CD:/"); check("CD:/ then unmounts it", status15() == "00, OK,00,00\r" && !vdUnit[0].mounted && vdUnit[0].cwd[0] == 0);
    IO1[rWRegCurrMenuWAIT] = rmtUSBDrive; strcpy(DriveDirPath, "/USB.D64*");
    VDrive_PreStart();
    check("menu image on USB auto-mounted on unit 1", vdUnit[1].mounted && !vdUnit[0].mounted && fast_load(9, "$").find("USBDISK") != std::string::npos);
    IO1[rWRegCurrMenuWAIT] = rmtSD; strcpy(DriveDirPath, "/GONE.D64*");
    VDrive_PreStart();
    check("missing image: no mount, no stray cwd, no file created", !vdUnit[0].mounted && vdUnit[0].cwd[0] == 0 && !fs_::exists(SD.root / "GONE.D64"));
    check("... the USB unit still restores its own remembered image", vdUnit[1].mounted);

    // ---- remembered mounts (/.vdrive sidecar on each unit's own card) ----
    fs_::remove(SD.root / ".vdrive"); fs_::remove(firstPartition.root / ".vdrive");
    IO1[rWRegCurrMenuWAIT] = rmtTeensy; strcpy(DriveDirPath, "/");
    cmd15("CD:SUB"); cmd15("CD:INNER.D64"); status15();                               // fresh unit 0 state after the last PreStart
    check("a mount writes the dot-file sidecar", fs_::exists(SD.root / ".vdrive"));
    { std::ifstream f(SD.root / ".vdrive"); std::string t((std::istreambuf_iterator<char>(f)), {}); check("sidecar holds the image path", t == "/sub/inner.d64"); }
    check("sidecar is not in the host listing", (cmd15("CD:_"), fast_load(8, "$").find(".VDRIVE") == std::string::npos));
    check("unmounting removes the sidecar", !fs_::exists(SD.root / ".vdrive"));
    cmd15("CD:/"); cmd15("CD:GAME.D64"); status15();
    VDrive_PreStart();                                                                  // power cycle, launched from a non-disk menu
    check("restart re-mounts the remembered SD image", vdUnit[0].mounted && vdUnit[0].cwd[0] == 0 && fast_load(8, "$").find("GAMEDISK") != std::string::npos);
    check("... and the USB unit has none (never mounted since reset)", !vdUnit[1].mounted);
    IO1[rWRegCurrMenuWAIT] = rmtSD; strcpy(DriveDirPath, "/SUB/INNER.D64*");
    VDrive_PreStart();
    check("a menu image beats the remembered one, and becomes the remembered one", strcmp(vdUnit[0].cwd, "/SUB") == 0 && vdUnit[0].mounted);
    IO1[rWRegCurrMenuWAIT] = rmtTeensy;
    VDrive_PreStart();
    check("... on the following restart too", strcmp(vdUnit[0].cwd, "/SUB") == 0 && vdUnit[0].mounted);
    fs_::remove(SD.root / "SUB" / "INNER.D64");
    VDrive_PreStart();
    check("remembered image deleted: starts unmounted, cwd reset", !vdUnit[0].mounted && vdUnit[0].cwd[0] == 0 && !fs_::exists(SD.root / ".vdrive"));

    // ---- D71 / D81 images and paths in names ----
    EEPROM.m[eepAdVDriveDevs] = 0xFF; EEPROM.m[eepAdVDriveDevs + 1] = 0xFF;
    IO1[rWRegCurrMenuWAIT] = rmtTeensy; strcpy(DriveDirPath, "/");
    VDrive_PreStart();
    cmd15("CD:/"); status15();
    cmd15("CD:BIG.D81"); check("D81 mounts", status15() == "00, OK,00,00\r" && vdUnit[0].mounted && vdUnit[0].d.fmt == D64_FMT_D81 && vdUnit[0].d.tracks == 80);
    check("D81 listing", fast_load(8, "$").find("BIGDISK") != std::string::npos);
    check("fast load from a D81", fast_load(8, "H81", &rc) == std::string("\x01\x08") + "d81!" && rc == 0);
    check("fast save to a D81", fast_save(8, "SAVED81", std::string(5000, 'R')) == 0);
    g_ms += 1000; PollingHndlr_VDrive();
    cmd15("CD:/");
    d = fast_load(8, "$/TWO.D71");
    check("LOAD\"$/TWO.D71\" mounts the D71 and lists it", d.find("TWODISK") != std::string::npos && vdUnit[0].mounted && vdUnit[0].d.fmt == D64_FMT_D71);
    check("... 1328 blocks free on a blank 1571 side-by-side count", d.find("BLOCKS FREE") != std::string::npos);
    check("path through an image: LOAD\"/BIG.D81/H81\"", fast_load(8, "/BIG.D81/H81", &rc) == std::string("\x01\x08") + "d81!" && rc == 0 && vdUnit[0].d.fmt == D64_FMT_D81);
    check("a saved D81 file is on the image after the flush (reopen)", (cmd15("CD:/"), cmd15("CD:BIG.D81"), fast_load(8, "SAVED81", &rc).size() == 5002));
    cmd15("UI"); check("UI answers 73", status15().rfind("73,", 0) == 0);
    cmd15("CD:/"); status15();

    // ---- bus scan: the fake KERNAL finds real drives at 8 and 9 and the glue hands out 10 / 11 on the first transaction ----
    g_real_drives = {8, 9};
    VDrive_PreStart();
    check("scan pending after start-up", vio->scan_state == 1 && r1(VIO_DEV0) == 8 && r1(VIO_DEV1) == 9);
    g_scans = 0;
    nm = call(VD_LISTEN, 8);
    check("first transaction triggered exactly one scan", g_scans == 1);
    check("real drive 8 is not ours any more", (nm & 0x100) != 0);
    call(VD_UNLSN);
    check("SD drive now 10, USB drive 11 (registers and unit numbers)", r1(VIO_DEV0) == 10 && r1(VIO_DEV1) == 11 && vio->vd.dev[0] == 10 && vio->vd.dev[1] == 11);
    check("the automatic numbers were not written to EEPROM", EEPROM.m[eepAdVDriveDevs] == 0xFF && EEPROM.m[eepAdVDriveDevs + 1] == 0xFF);
    check("device 10 serves the SD card", fast_load(10, "$").find("\"SD:/") != std::string::npos);
    g_real_drives.clear();
    VDrive_PreStart(); call(VD_LISTEN, 8); call(VD_UNLSN);
    check("empty bus again: back to 8 / 9", r1(VIO_DEV0) == 8 && r1(VIO_DEV1) == 9);
    EEPROM.m[eepAdVDriveDevs] = 12; EEPROM.m[eepAdVDriveDevs + 1] = 0;           // SD fixed to 12, USB automatic
    g_real_drives = {8};
    VDrive_PreStart(); g_scans = 0; call(VD_LISTEN, 12); call(VD_UNLSN);
    check("fixed SD number is kept, USB takes the first free one (9)", g_scans == 1 && r1(VIO_DEV0) == 12 && r1(VIO_DEV1) == 9);
    EEPROM.m[eepAdVDriveDevs] = 12; EEPROM.m[eepAdVDriveDevs + 1] = 13;
    VDrive_PreStart(); g_scans = 0; call(VD_LISTEN, 12); call(VD_UNLSN);
    check("both fixed: no scan at all", g_scans == 0 && r1(VIO_DEV0) == 12 && r1(VIO_DEV1) == 13);

    printf("%s\n", fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
