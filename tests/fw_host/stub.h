// Minimal stand-ins for the Teensy/TeensyROM environment so firmware/IOH_TDrive.c can be
// compiled and exercised on the host. NOT the real headers: this checks the glue's logic
// and C++ validity, not the Teensy toolchain build.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>

#define FLASHMEM
#define FASTRUN
#define DMAMEM
#define PROGMEM
#define FILE_READ  0
#define FILE_WRITE 1
#define Printf_dbg(...) do {} while (0)
#define IOHNameLength 20

namespace fs_ = std::filesystem;

struct File {
    struct St { FILE *f = nullptr; fs_::path p; bool dir = false; std::vector<fs_::path> kids; size_t next = 0; std::string nm; };
    std::shared_ptr<St> s;
    explicit operator bool() const { return (bool)s; }
    bool isDirectory() const { return s && s->dir; }
    bool seek(uint32_t o) { return s && s->f && fseek(s->f, (long)o, SEEK_SET) == 0; }
    int read(void *b, size_t n) { return (int)fread(b, 1, n, s->f); }
    size_t write(const void *b, size_t n) { return fwrite(b, 1, n, s->f); }
    void flush() { if (s && s->f) fflush(s->f); }
    void close() { if (s && s->f) { fclose(s->f); s->f = nullptr; } s.reset(); }
    uint32_t size() const { return (uint32_t)fs_::file_size(s->p); }
    const char *name() const { return s->nm.c_str(); }
    File openNextFile() {
        File r;
        if (!s || !s->dir || s->next >= s->kids.size()) return r;
        fs_::path p = s->kids[s->next++];
        r.s = std::make_shared<St>(); r.s->p = p; r.s->nm = p.filename().string(); r.s->dir = fs_::is_directory(p);
        return r;
    }
};

struct FS;
struct FsVolume { FS *fs; };                        // SdFat's volume: SD.sdfs, firstPartition.mscfs
struct FS {
    fs_::path root;
    FsVolume sdfs{this}, mscfs{this};
    // FAT is case-insensitive; so are the macOS hosts this was written on. Resolve each component by a case-insensitive match
    // so the same tests run on a case-sensitive host file system. A missing last component keeps the requested spelling.
    fs_::path resolve(const char *path) const {
        fs_::path cur = root;
        std::string p = path[0] == '/' ? path + 1 : path;
        size_t i = 0;
        while (i < p.size()) {
            size_t j = p.find('/', i); if (j == std::string::npos) j = p.size();
            std::string part = p.substr(i, j - i); i = j + 1;
            if (part.empty()) continue;
            fs_::path direct = cur / part;
            if (!fs_::exists(direct) && fs_::is_directory(cur)) {
                for (auto &e : fs_::directory_iterator(cur)) {
                    std::string n = e.path().filename().string();
                    if (n.size() == part.size() && std::equal(n.begin(), n.end(), part.begin(), [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); })) { direct = e.path(); break; }
                }
            }
            cur = direct;
        }
        return cur;
    }
    bool exists(const char *path) { return fs_::exists(resolve(path)); }
    bool remove(const char *path) { return fs_::remove(resolve(path)); }
    bool mkdir(const char *path) { std::error_code e; return fs_::create_directory(resolve(path), e); }
    bool rmdir(const char *path) {                        // like SdFat: only an empty folder
        fs_::path p = resolve(path); std::error_code e;
        return fs_::is_directory(p) && fs_::is_empty(p) && fs_::remove(p, e);
    }
    File open(const char *path, int mode = FILE_READ) {
        File r; fs_::path p = resolve(path);
        if (!fs_::exists(p)) {
            if (mode != FILE_WRITE) return r;
            FILE *nf = fopen(p.c_str(), "w+b");           // real SD: FILE_WRITE = O_RDWR|O_CREAT, creates the file
            if (!nf) return r;
            fclose(nf);
        }
        r.s = std::make_shared<File::St>(); r.s->p = p; r.s->nm = p.filename().string();
        if (fs_::is_directory(p)) {
            r.s->dir = true;
            for (auto &e : fs_::directory_iterator(p)) r.s->kids.push_back(e.path());
            std::sort(r.s->kids.begin(), r.s->kids.end());
        } else {
            r.s->f = fopen(p.c_str(), mode == FILE_WRITE ? "r+b" : "rb");
            if (!r.s->f) r.s.reset();
        }
        return r;
    }
};
extern FS SD, firstPartition;

// SdFat's FsFile, as the glue uses it for a mounted image: sync() and close() say whether the writes reached the card
#define O_RDONLY 0
#define O_RDWR 2
extern bool g_sync_fail;                               // test hook: the card fails every sync
struct FsFile {
    FILE *f = nullptr;
    FsFile() = default;
    FsFile(const FsFile &) = delete;
    FsFile &operator=(const FsFile &) = delete;
    ~FsFile() { close(); }
    bool open(FsVolume *vol, const char *path, int oflag) {
        close();
        if (!vol) return false;
        fs_::path p = vol->fs->resolve(path);
        if (!fs_::is_regular_file(p)) return false;          // no O_CREAT here: a missing file is not created
        f = fopen(p.c_str(), oflag == O_RDWR ? "r+b" : "rb");
        return f != nullptr;
    }
    bool isOpen() const { return f != nullptr; }
    bool seekSet(uint64_t o) { return f && fseek(f, (long)o, SEEK_SET) == 0; }
    int read(void *b, size_t n) { return f ? (int)fread(b, 1, n, f) : -1; }
    size_t write(const void *b, size_t n) { return f ? fwrite(b, 1, n, f) : 0; }
    bool sync() { return f && fflush(f) == 0 && !g_sync_fail; }
    bool close() { if (!f) return true; bool ok = sync(); fclose(f); f = nullptr; return ok; }
};

struct SerialStub { void println(const char *s) { puts(s); } template <class... A> void printf(const char *f, A... a) { ::printf(f, a...); } void flush() { fflush(stdout); } };
// the Alt-button trace's CrashReport breadcrumbs / fault record / reset reason (IOH_TDrive.c VD_Trace*): plain arrays here
extern volatile uint32_t g_vd_bc[8], g_vd_fault[11];
extern uint32_t g_vd_srsr;
#define VD_TRACE_BC    (g_vd_bc)
#define VD_TRACE_FAULT (g_vd_fault)
#define VD_TRACE_SRSR  (g_vd_srsr)
inline void arm_dcache_flush(void *, uint32_t) {}
extern SerialStub Serial;
uint32_t millis();
uint32_t micros();
extern bool g_led;                                    // pin 34, the TR's LED (SetLEDOn = high)
inline bool digitalReadFast(uint8_t pin) { return pin == 34 && g_led; }
inline void digitalWriteFast(uint8_t pin, bool v) { if (pin == 34) g_led = v; }

struct stcIOHandlers {
    char Name[IOHNameLength];
    void (*InitHndlr)();
    void (*IO1Hndlr)(uint8_t, bool);
    void (*IO2Hndlr)(uint8_t, bool);
    void (*ROMLHndlr)(uint32_t, bool);
    void (*ROMHHndlr)(uint32_t, bool);
    void (*PollingHndlr)();
    void (*CycleHndlr)(bool);
};

struct EEPROMStub { uint8_t m[8192]; uint8_t read(int a) { return m[a]; } void write(int a, uint8_t v) { m[a] = v; } };
extern EEPROMStub EEPROM;
enum { eepAdTDriveDevs = 4235, eepAdNextIOHndlr = 6 };
#ifndef PLAIN_TR                                      // -DPLAIN_TR: the plain TeensyROM (no Fab04_*: no KERNAL handlers, REU, Special button)
#define Fab04_REU
#define Fab04_TDrive
#ifndef VD_TEST_NO_DMA                                // run.py also builds the glue without it: the window LOAD only (no DMA hook)
#define Fab04_TDriveDMALoad
#endif
#endif
#define TDrive_Agent
enum { IOH_None = 0, IOH_Swiftlink, IOH_REU, IOH_TDrive, IOH_TDriveSwift, IOH_TDriveSwiftREU, IOH_TDriveREU, IOH_TDriveAgent, IOH_Other };   // only the order within the real enum is irrelevant here
extern bool (*fBusSnoop)(uint16_t Address, bool R_Wn);
bool KernalCheck(uint16_t Address, bool R_Wn);
extern uint8_t HIRAM_State;
enum { HIRAM_State_Unknown = 0, HIRAM_State_ROM, HIRAM_State_RAM };
extern volatile uint8_t KDS_State;                    // IOH_KernalReplace.c: the C128's DMA HIRAM sense
extern bool KDS_Read01Valid;
enum { KDS_Idle = 0 };
bool (*KernalCheckArm())(uint16_t Address, bool R_Wn); // the check a launch hooks (KernalCheck on a C64)
extern volatile uint8_t BtnPressed;

enum { rwRegPwrUpDefaults2 = 8, rpud2AltBtnActionMask = 0b00111000, rpud2AltBtnAutoLaunch = 0, rpud2AltBtnPause = 0b00001000, rpud2AltBtnNone = 0b00100000 };
#ifndef PLAIN_TR
#define Fab04_SpecialButton
#endif
extern void (*fSpecialBtnChange)(bool Up_nDn);
enum { rWRegCurrMenuWAIT = 3, rwRegScratch = 9, rwRegNextIOHndlr = 22, rwRegColorRefStart = 42, NumColorRefs = 7, rmtUSBDrive = 0, rmtSD = 1, rmtTeensy = 2 };
enum { IO1Size = 104 };   // Menu_Regs.h: StartSIDRegs + 40
extern volatile uint8_t *IO1;
extern char DriveDirPath[];

// DMAControl.c: reads of the C64's memory (main.cpp serves g_c64_kernal at $E000)
enum DMA_Trans_RnW { DMA_READ = true, DMA_WRITE = false };
enum DMA_Addr_Mode { DMA_ADDR_FIXED = true, DMA_ADDR_INCREMENT = false };
bool PerformDMA(DMA_Trans_RnW RnW, uint16_t StartAddr, uint8_t *Buffer, uint32_t Length, DMA_Addr_Mode FixC64Addr);
bool CloseDMA();
// a held transfer (HoldC64, SetUpDMA, DMA_State = DMA_S_TransferExecuting): main.cpp runs it when the state is set, as isrPHI2 would
enum { DMA_S_DisableReady = 0, DMA_S_TransferReady, DMA_S_TransferExecuting, DMA_S_TransferComplete };
void SetUpDMA(DMA_Trans_RnW RnW, uint16_t StartAddr, uint8_t *Buffer, uint32_t Length, DMA_Addr_Mode FixC64Addr);
bool HoldC64();
void DmaStubRun();
struct DmaStateStub {
    uint8_t v = DMA_S_DisableReady;
    DmaStateStub &operator=(int x) { v = (uint8_t)x; if (x == DMA_S_TransferExecuting) DmaStubRun(); return *this; }
    operator int() const { return v; }
};
extern DmaStateStub DMA_State;
extern bool isFrozen;

inline bool IsDotfile(const char *n) { return n[0] == '.'; }
bool SDFullInit();
bool USBFileSystemWait();
void InitHndlr_KernalReplace();

// bus data port: tests set/read these
extern uint8_t g_bus_in, g_bus_out;
inline void DataPortWriteWaitLog(uint8_t d) { g_bus_out = d; }
inline uint8_t DataPortWaitRead() { return g_bus_in; }
inline void TraceLogAddValidData(uint8_t) {}
