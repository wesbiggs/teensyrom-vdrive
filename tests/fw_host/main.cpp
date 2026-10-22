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
void (*fSpecialBtnChange)(bool Up_nDn) = nullptr;
char DriveDirPath[128];
EEPROMStub EEPROM;
uint8_t g_bus_in, g_bus_out;
static uint32_t g_ms = 0, g_tick = 0;               // g_tick > 0: every millis() call advances the clock (for code that waits)
static void (*g_on_millis)() = nullptr;               // test hook: a busy C64 issuing mailbox commands while the firmware waits
uint32_t millis() { g_ms += g_tick; if (g_on_millis) g_on_millis(); return g_ms; }
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
    IO1Hndlr_VDrive(VD_MboxBase + 9, true);
    check("mailbox is not mapped at $DE00-$DE07 or past $DE18", g_bus_out == 0x77);
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
    check("LOAD\"$/TWO.D71\" lists the D71 and leaves nothing mounted", d.find("TWODISK") != std::string::npos && !vdUnit[0].mounted && vdUnit[0].cwd[0] == 0);
    check("... and the sidecar was not touched by the temporary mount", !fs_::exists(SD.root / ".vdrive"));
    d = fast_load(8, "$==/TWO.D71");
    check("LOAD\"$==/TWO.D71\" mounts it for good", d.find("TWODISK") != std::string::npos && vdUnit[0].mounted && vdUnit[0].d.fmt == D64_FMT_D71 && fs_::exists(SD.root / ".vdrive"));
    cmd15("CD:/");
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

    // ---- remote control: VDrive_RemoteStatus / VDrive_RemoteSet (the TCP 2112 get / set commands) ----
    {
        g_real_drives.clear();
        EEPROM.m[eepAdVDriveDevs] = 0xFF; EEPROM.m[eepAdVDriveDevs + 1] = 0xFF;
        fs_::remove(SD.root / ".vdrive"); fs_::remove(firstPartition.root / ".vdrive");
        IO1[rWRegCurrMenuWAIT] = rmtTeensy; DriveDirPath[0] = 0;
        VDrive_PreStart();
        fs_::copy_file(SD.root / "TWO.D71", SD.root / "SUB" / "COPY.D64", fs_::copy_options::overwrite_existing);   // a 1571 image under a .D64 name: the size decides
        char js[300], msg[64];
        auto has = [&](int unit, const char *needle) { return VDrive_RemoteStatus(unit, js, sizeof js) > 0 && strstr(js, needle) != nullptr; };
        check("remote get: SD unit, nothing mounted", has(0, "\"unit\":0,\"storage\":\"sd\",\"device\":8,\"fixed\":0,\"mounted\":false"));
        check("remote get: USB unit is device 9", has(1, "\"storage\":\"usb\",\"device\":9"));
        check("remote get: no unit 2", VDrive_RemoteStatus(2, js, sizeof js) == 0 && VDrive_RemoteStatus(-1, js, sizeof js) == 0);
        check("remote get: output too small is refused, not truncated", VDrive_RemoteStatus(0, js, 20) == 0);

        g_tick = 1;                                           // let the waiting code see time pass
        check("remote set: mounts an image on SD", VDrive_RemoteSet(0, "/GAME.D64", msg, sizeof msg) == 0 && vdUnit[0].mounted);
        check("remote get: shows it", has(0, "\"mounted\":true,\"writable\":true,\"path\":\"/game.d64\",\"cwd\":\"/\""));
        g_tick = 0;
        int rc = -1;
        check("... and the C64 can read from it", fast_load(8, "HELLO", &rc) == hello_expect && rc == 0);
        g_tick = 1;

        check("remote set: unknown image fails with 62 and keeps the current disk", VDrive_RemoteSet(0, "/NOPE.D64", msg, sizeof msg) == 62 && strstr(msg, "not found") && has(0, "/game.d64"));
        check("remote set: a directory is not an image (74)", VDrive_RemoteSet(0, "/SUB", msg, sizeof msg) == 74 && has(0, "/game.d64"));
        check("remote set: relative path refused (33)", VDrive_RemoteSet(0, "GAME.D64", msg, sizeof msg) == 33 && has(0, "/game.d64"));
        check("remote set: no such unit", VDrive_RemoteSet(2, "/GAME.D64", msg, sizeof msg) == 1);
        check("remote set: no file was created by the failed attempts", !fs_::exists(SD.root / "NOPE.D64"));

        check("remote set: other image in a folder", VDrive_RemoteSet(0, "/SUB/COPY.D64", msg, sizeof msg) == 0 && has(0, "\"path\":\"/SUB/copy.d64\",\"cwd\":\"/SUB\""));
        check("... the C64 sees the new disk", (g_tick = 0, fast_load(8, "$").find("TWODISK") != std::string::npos));
        g_tick = 1;
        { std::ifstream f(SD.root / ".vdrive"); std::string t((std::istreambuf_iterator<char>(f)), {}); check("... and it is the remembered one", t == "/SUB/copy.d64"); }
        check("remote set: D81 works too", VDrive_RemoteSet(0, "/BIG.D81", msg, sizeof msg) == 0 && vdUnit[0].d.fmt == D64_FMT_D81);

        check("remote set: USB unit", VDrive_RemoteSet(1, "/USB.D64", msg, sizeof msg) == 0 && has(1, "\"path\":\"/usb.d64\"") && has(0, "/big.d81"));

        // quiet point: a mailbox command just happened, so the swap waits at least VD_QuietMs
        g_tick = 0; call(VD_LISTEN, 8); call(VD_SECOND, 0x6F); call(VD_UNLSN); lastActivityMs = millis();
        g_tick = 1; uint32_t t0 = g_ms;
        check("remote set: waits for a quiet moment after C64 activity", VDrive_RemoteSet(0, "/GAME.D64", msg, sizeof msg) == 0 && g_ms - t0 >= VD_QuietMs);

        // a LOAD in progress holds the swap off until it is abandoned (VD_StaleMs), then the open transfer is dropped
        g_tick = 0; put_name("HELLO"); call(VIO_FLOAD_OPEN, 8, 5); lastActivityMs = millis();
        check("fast LOAD is open", vio->fl_active);
        g_tick = 1; t0 = g_ms;
        check("remote set: a LOAD in progress delays it until the transfer is stale", VDrive_RemoteSet(0, "/SUB/COPY.D64", msg, sizeof msg) == 0 && g_ms - t0 >= VD_StaleMs);

        // an open file channel does not block the swap but is closed by it
        g_tick = 0; cmd15("CD:/"); cmd15("CD:GAME.D64"); status15();
        call(VD_LISTEN, 8); call(VD_SECOND, 0xF2); for (char c : std::string("HELLO")) call(VD_CIOUT, (uint8_t)c); call(VD_UNLSN);
        check("a file channel is open", vdrive_chan_mode(&vio->vd, 0, 2) == 1);
        g_tick = 1;
        check("remote set closes the channels on the unit", VDrive_RemoteSet(0, "/SUB/COPY.D64", msg, sizeof msg) == 0 && vdrive_chan_mode(&vio->vd, 0, 2) == 0);

        g_tick = 0; check("the drive status channel is clean after a swap", status15() == "00, OK,00,00\r"); g_tick = 1;
        check("remote set: empty path unmounts, folder stays, sidecar gone", VDrive_RemoteSet(0, "", msg, sizeof msg) == 0 && !vdUnit[0].mounted && !fs_::exists(SD.root / ".vdrive") && has(0, "\"mounted\":false,\"writable\":false,\"path\":\"\",\"cwd\":\"/SUB\""));
        g_tick = 0;
        check("... the C64 lists the host folder again", fast_load(8, "$").find("\"SD:/SUB") != std::string::npos);

        // the remote directory listing (and the swap) wait for the drive to be quiet; a C64 that never lets go gets a refusal
        g_tick = 0; call(VD_LISTEN, 8); call(VD_UNLSN); lastActivityMs = millis();
        g_tick = 1; t0 = g_ms;
        check("quiet wait: returns 0 after the C64's last command has aged", VDrive_RemoteQuiet(msg, sizeof msg) == 0 && g_ms - t0 >= VD_QuietMs && msg[0] == 0);
        t0 = g_ms;
        check("quiet wait: already quiet returns at once", VDrive_RemoteQuiet(msg, sizeof msg) == 0 && g_ms - t0 < 5);
        VDrive_RemoteSet(0, "/GAME.D64", msg, sizeof msg);
        g_on_millis = [] { if (!vioHot.pending) { w1(VIO_LEN, 0); w1(VIO_ARG, 0); w1(VIO_CMD, VD_UNLSN); } };   // never lets go
        t0 = g_ms;
        check("quiet wait: a C64 that keeps talking is refused after the limit", VDrive_RemoteQuiet(msg, sizeof msg) == 2 && strstr(msg, "busy") && g_ms - t0 >= VD_SwapWaitMs);
        check("remote set in the same situation fails (2) and leaves the disk alone", VDrive_RemoteSet(0, "/BIG.D81", msg, sizeof msg) == 2 && has(0, "/game.d64"));
        g_on_millis = nullptr; g_tick = 0;
        vioReady = false;
        check("quiet wait: VDrive not running is refused", VDrive_RemoteQuiet(msg, sizeof msg) == 1);
        vioReady = true;
    }

    // ---- remote control with the TR menu showing (VDrive not running): the remembered image in /.vdrive ----
    {
        char js[400], msg[64];
        EEPROM.m[eepAdVDriveDevs] = 0xFF; EEPROM.m[eepAdVDriveDevs + 1] = 12;
        fs_::remove(SD.root / ".vdrive"); fs_::remove(firstPartition.root / ".vdrive");
        auto idle = [&](int unit, const char *needle) { return VDrive_RemoteIdleStatus(unit, js, sizeof js) > 0 && strstr(js, needle) != nullptr; };
        check("idle get: nothing remembered", idle(0, "\"running\":false,\"device\":0,\"fixed\":0,\"mounted\":false,\"writable\":true,\"path\":\"\",\"cwd\":\"/\""));
        check("idle get: a fixed device number is reported", idle(1, "\"storage\":\"usb\",\"running\":false,\"device\":12,\"fixed\":12"));
        check("idle get: no unit 2", VDrive_RemoteIdleStatus(2, js, sizeof js) == 0 && VDrive_RemoteIdleStatus(-1, js, sizeof js) == 0);
        check("idle set: remembers an image on SD", VDrive_RemoteIdleSet(0, "/SUB/COPY.D64", msg, sizeof msg) == 0 && idle(0, "\"mounted\":true,\"writable\":true,\"path\":\"/SUB/COPY.D64\",\"cwd\":\"/SUB\""));
        { std::ifstream f(SD.root / ".vdrive"); std::string t((std::istreambuf_iterator<char>(f)), {}); check("... the sidecar holds exactly that path", t == "/SUB/COPY.D64"); }
        check("idle set: a doubled slash is squeezed", VDrive_RemoteIdleSet(1, "//USB.D64", msg, sizeof msg) == 0 && idle(1, "\"path\":\"/USB.D64\",\"cwd\":\"/\""));
        check("idle set: unknown image 62, sidecar untouched", VDrive_RemoteIdleSet(0, "/NOPE.D64", msg, sizeof msg) == 62 && idle(0, "/SUB/COPY.D64"));
        check("idle set: a folder is not an image (74)", VDrive_RemoteIdleSet(0, "/SUB", msg, sizeof msg) == 74 && idle(0, "/SUB/COPY.D64"));
        check("idle set: relative path refused (33)", VDrive_RemoteIdleSet(0, "GAME.D64", msg, sizeof msg) == 33 && idle(0, "/SUB/COPY.D64"));
        check("idle set: no such unit", VDrive_RemoteIdleSet(2, "/GAME.D64", msg, sizeof msg) == 1);
        fs_::remove(SD.root / "SUB" / "COPY.D64");
        check("idle get: a remembered image that is gone shows as unmounted", idle(0, "\"mounted\":false"));
        fs_::copy_file(SD.root / "TWO.D71", SD.root / "SUB" / "COPY.D64", fs_::copy_options::overwrite_existing);
        check("idle set: empty path forgets the image", VDrive_RemoteIdleSet(0, "", msg, sizeof msg) == 0 && !fs_::exists(SD.root / ".vdrive") && idle(0, "\"mounted\":false"));
        // the next start mounts what was set, and a doubled slash in a remembered path no longer leaks into the display
        VDrive_RemoteIdleSet(0, "/GAME.D64", msg, sizeof msg);
        IO1[rWRegCurrMenuWAIT] = rmtTeensy; DriveDirPath[0] = 0;
        VDrive_PreStart();
        check("idle set then start: the image is mounted", vdUnit[0].mounted && fast_load(8, "$").find("GAMEDISK") != std::string::npos);
        { std::ofstream f(firstPartition.root / ".vdrive", std::ios::trunc); f << "//USB.D64"; }
        VDrive_PreStart();
        check("a sidecar with a doubled slash mounts with cwd root, not //", vdUnit[1].mounted && vdUnit[1].cwd[0] == 0);
        check("... and the status path has a single slash", VDrive_RemoteStatus(1, js, sizeof js) > 0 && strstr(js, "\"path\":\"/usb.d64\"") && !strstr(js, "//"));
        fs_::remove(SD.root / ".vdrive"); fs_::remove(firstPartition.root / ".vdrive");
    }

    // ---- alt button: cycle through /vdrive-config.txt, flash the border ----
    {
        auto put = [&](FS &fs, const char *name, const std::string &text) { std::ofstream(fs.root / name, std::ios::binary) << text; };
        auto cur = [&](int unit) { char js[300]; VDrive_RemoteStatus(unit, js, sizeof js); std::string j = js; size_t a = j.find("\"path\":\""); std::string r = j.substr(a + 8, j.find('"', a + 8) - a - 8); for (auto &c : r) c = (char)tolower((unsigned char)c); return r; };
        typedef std::vector<std::pair<uint32_t, uint8_t>> Trace;
        // the Alt button wiring: only when the setting is "None"
        IO1[rwRegPwrUpDefaults2] = rpud2AltBtnPause; fSpecialBtnChange = nullptr;
        InitHndlr_VDrive();
        check("alt: a chosen action (Pause) is left alone", fSpecialBtnChange == nullptr);
        IO1[rwRegPwrUpDefaults2] = rpud2AltBtnAutoLaunch;
        InitHndlr_VDrive();
        check("alt: AutoLaunch is left alone too", fSpecialBtnChange == nullptr);
        IO1[rwRegPwrUpDefaults2] = rpud2AltBtnNone;
        InitHndlr_VDrive();
        check("alt: setting None -> VDrive takes the button", fSpecialBtnChange == &SpecialBtn_VDrive);

        fs_::create_directories(SD.root / "SUB");
        fs_::copy_file(SD.root / "GAME.D64", SD.root / "SUB" / "INNER.D64", fs_::copy_options::overwrite_existing);   // earlier tests removed it
        // fresh state: nothing mounted, sidecars gone
        fs_::remove(SD.root / ".vdrive"); fs_::remove(firstPartition.root / ".vdrive");
        IO1[rWRegCurrMenuWAIT] = rmtTeensy; DriveDirPath[0] = 0;
        VDrive_PreStart(); InitHndlr_VDrive();
        check("border register idles at $80", vioHot.border == VIO_BRD_IDLE && r1(VIO_BORDER) == 0x80);
        g_ms += 5000;

        auto run = [&](uint32_t ms) { Trace t; uint8_t last = vioHot.border; uint32_t t0 = g_ms;
                                      for (uint32_t i = 0; i < ms; i += 5) { g_ms += 5; PollingHndlr_VDrive(); if (vioHot.border != last) { last = vioHot.border; t.push_back({g_ms - t0, last}); } }
                                      return t; };
        auto near = [](uint32_t a, uint32_t b) { return a + 20 >= b && a <= b + 20; };
        auto tap = [&](int n) { for (int i = 0; i < n; i++) { SpecialBtn_VDrive(false); g_ms += 50; SpecialBtn_VDrive(true); g_ms += 100; } };
        // a full sequence = SAVE, then (SET c, RESTORE) per flash, then IDLE
        auto seq_ok = [&](const Trace &t, uint8_t c, int flashes) {
            if ((int)t.size() != 2 + 2 * flashes + 1 - 1 + 0 && (int)t.size() != 2 * flashes + 2) return false;
            if (t[0].second != VIO_BRD_SAVE) return false;
            for (int i = 0; i < flashes; i++) {
                if (t[1 + 2 * i].second != (VIO_BRD_SET | c) || t[2 + 2 * i].second != VIO_BRD_RESTORE) return false;
            }
            return t.back().second == VIO_BRD_IDLE;
        };
        auto flash_trace = [&](uint32_t wait) { Trace t = run(wait); return t; };
        (void)flash_trace;

        put(SD, "vdrive-config.txt",
            "# my disks\r\n"
            "/GAME.D64, 3, 2\r\n"
            "/MISSING.D64, 4, 1\r\n"
            "not a path\r\n"
            "/SUB/INNER.D64,5\r\n"
            "\r\n"
            "  /BIG.D81 , 6 , 3  \r\n"
            "/TWO.D71\r\n");
        check("alt: nothing mounted yet", cur(0) == "");

        // single tap: waits out the 1 s double-tap window, then picks the first entry
        tap(1);
        Trace t = run(500);
        check("alt: one tap does nothing before the window closes", t.empty() && cur(0) == "");
        t = run(6000);
        check("alt: first entry mounted (nothing was mounted)", cur(0) == "/game.d64");
        check("alt: 2 flashes in colour 3 (save, set, restore, set, restore, idle)", seq_ok(t, 3, 2) && t.size() == 6);
        check("alt: flash timing 100 ms settle, 250 on, 750 off, 250 on, 100 settle",
              t.size() == 6 && near(t[1].first - t[0].first, 100) && near(t[2].first - t[1].first, 250) && near(t[3].first - t[2].first, 750) &&
              near(t[4].first - t[3].first, 250) && near(t[5].first - t[4].first, 100));
        check("alt: USB untouched", cur(1) == "");

        tap(1); t = run(3000);
        check("alt: next entry skips the missing image and the junk line (INNER, colour 5, default 1 flash)", cur(0) == "/sub/inner.d64" && seq_ok(t, 5, 1));
        check("alt: the folder became the cwd", [&] { char js[300]; VDrive_RemoteStatus(0, js, sizeof js); return strstr(js, "\"cwd\":\"/SUB\"") != nullptr; }());
        tap(1); t = run(5000);
        check("alt: next: the padded line (BIG.D81, colour 6, 3 flashes)", cur(0) == "/big.d81" && seq_ok(t, 6, 3));
        tap(1); t = run(3000);
        check("alt: next: last line without numbers (TWO.D71, colour 1, 1 flash)", cur(0) == "/two.d71" && seq_ok(t, 1, 1));
        tap(1); t = run(5000);
        check("alt: wraps around to the first entry", cur(0) == "/game.d64" && seq_ok(t, 3, 2));

        // the mounted image is not in the list: start again at the first entry
        put(SD, "vdrive-config.txt", "/SUB/INNER.D64,7,1\n/BIG.D81,8,1\n");
        tap(1); t = run(3000);
        check("alt: mounted image not in the list -> first entry", cur(0) == "/sub/inner.d64" && seq_ok(t, 7, 1));
        // a vanished image is skipped, also when it is the one after the current
        fs_::rename(SD.root / "BIG.D81", SD.root / "BIG.OFF");
        tap(1); t = run(3000);
        check("alt: only the current image is left: it is selected again", cur(0) == "/sub/inner.d64" && seq_ok(t, 7, 1));
        fs_::rename(SD.root / "BIG.OFF", SD.root / "BIG.D81");

        // errors flash red, quickly, for 2 s
        auto red_ok = [&](const Trace &t) {
            if (t.size() != 2 * VD_ErrFlashes + 2 - 1 + 0 && t.size() != 2 * VD_ErrFlashes + 2) return false;
            if (t[0].second != VIO_BRD_SAVE || t.back().second != VIO_BRD_IDLE) return false;
            for (int i = 0; i < VD_ErrFlashes; i++) if (t[1 + 2 * i].second != (VIO_BRD_SET | VD_ColorRed) || t[2 + 2 * i].second != VIO_BRD_RESTORE) return false;
            return t.back().first - t[0].first >= 1800 && t.back().first - t[0].first <= 2300;
        };
        put(SD, "vdrive-config.txt", "/MISSING.D64,4,1\n/ALSO/GONE.D64\n");
        tap(1); t = run(5000);
        check("alt: every image missing -> red flash for 2 s, current disk stays", red_ok(t) && cur(0) == "/sub/inner.d64");
        put(SD, "vdrive-config.txt", "nothing useful\n# here\n");
        tap(1); t = run(5000);
        check("alt: no usable line -> red", red_ok(t));
        put(SD, "vdrive-config.txt", "");
        tap(1); t = run(5000);
        check("alt: empty config -> red", red_ok(t));
        fs_::remove(SD.root / "vdrive-config.txt");
        tap(1); t = run(5000);
        check("alt: no config file -> red", red_ok(t));
        // USB: double tap, own list
        put(firstPartition, "vdrive-config.txt", "/USB.D64, 9, 1\n");
        put(SD, "vdrive-config.txt", "/GAME.D64,3,1\n");
        tap(2); t = run(3000);
        check("alt: two taps within 1 s act at once on the USB list", cur(1) == "/usb.d64" && seq_ok(t, 9, 1) && cur(0) == "/sub/inner.d64");
        // a tap while a long flash is still running: the colour is given back first (saving mid-flash would remember the flash colour)
        put(SD, "vdrive-config.txt", "/SUB/INNER.D64,7,1\n/GAME.D64,3,3\n");
        tap(1); run(1000);
        check("alt: the long sequence (GAME, 3 flashes) is under way", cur(0) == "/game.d64" && vioHot.border != VIO_BRD_IDLE);
        tap(1); t = run(8000);
        int lastSave = -1; for (int i = 0; i < (int)t.size(); i++) if (t[i].second == VIO_BRD_SAVE) lastSave = i;
        bool restoredBeforeSave = lastSave >= 1 && t[lastSave - 1].second == VIO_BRD_RESTORE && t[lastSave].first - t[lastSave - 1].first >= 100;
        Trace tail(t.begin() + (lastSave < 0 ? 0 : lastSave), t.end());
        check("alt: new tap during a flash: restore (held 100 ms), then the new sequence (INNER, 7)", restoredBeforeSave && seq_ok(tail, 7, 1) && cur(0) == "/sub/inner.d64");
        // missing stick: red
        vdrive_attach(&vio->vd, 1, NULL, 0);
        tap(2); t = run(5000);
        check("alt: no USB stick -> red", red_ok(t));
        vdrive_attach(&vio->vd, 1, NULL, 1);
        // C64 busy with the drive for good: red, nothing changes
        put(firstPartition, "vdrive-config.txt", "/USB.D64,9,1\n");
    }

    printf("%s\n", fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
