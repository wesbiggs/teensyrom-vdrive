// Minimal stand-ins for the Teensy/TeensyROM environment so firmware/IOH_VDrive.c can be
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

struct FS {
    fs_::path root;
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

struct SerialStub { void println(const char *s) { puts(s); } template <class... A> void printf(const char *f, A... a) { ::printf(f, a...); } };
extern SerialStub Serial;
uint32_t millis();

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
enum { eepAdVDriveDevs = 4235, eepAdNextIOHndlr = 6 };
#define Fab04_REU
enum { IOH_None = 0, IOH_Swiftlink, IOH_REU, IOH_VDrive, IOH_VDriveSwift, IOH_VDriveSwiftREU, IOH_Other };   // only the order within the real enum is irrelevant here
extern bool (*fBusSnoop)(uint16_t Address, bool R_Wn);

enum { rwRegPwrUpDefaults2 = 8, rpud2AltBtnActionMask = 0b00111000, rpud2AltBtnAutoLaunch = 0, rpud2AltBtnPause = 0b00001000, rpud2AltBtnNone = 0b00100000 };
#define Fab04_SpecialButton
extern void (*fSpecialBtnChange)(bool Up_nDn);
enum { rWRegCurrMenuWAIT = 3, rmtUSBDrive = 0, rmtSD = 1, rmtTeensy = 2 };
extern volatile uint8_t *IO1;
extern char DriveDirPath[];

inline bool IsDotfile(const char *n) { return n[0] == '.'; }
bool SDFullInit();
bool USBFileSystemWait();
void InitHndlr_KernalReplace();

// bus data port: tests set/read these
extern uint8_t g_bus_in, g_bus_out;
inline void DataPortWriteWaitLog(uint8_t d) { g_bus_out = d; }
inline uint8_t DataPortWaitRead() { return g_bus_in; }
inline void TraceLogAddValidData(uint8_t) {}
