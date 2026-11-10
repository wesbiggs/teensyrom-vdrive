// The VDrive Special IOs: the virtual IEC drive and its SD / USB glue. On the TeensyROM+ (Fab04_VDrive) "VDrive KERNAL" and its
// combined handlers serve a replacement KERNAL; on both boards (VDrive_Agent) IOH_VDriveAgent.c reaches the same drive through the
// KERNAL's RAM vectors and a 6502 agent (docs/DESIGN.md 53). The KERNAL-only parts are under Fab04_VDrive.
// "VDrive" Special IO for TeensyROM+: replacement KERNAL + virtual IEC drive.
//   unit 0 = SD card, unit 1 = USB drive. Device numbers are automatic by default: the first free numbers from 8 up
//   (the KERNAL probes the real serial bus on the first disk access of a run, see vdrive_io.h), so 8 (SD) and 9 (USB) when
//   no other drive is attached. A number can be fixed from BASIC with  OPEN15,8,15,"U0>"+CHR$(10):CLOSE15  (this drive becomes
//   device 10, kept in EEPROM; CHR$(0) makes it automatic again). Each unit serves a mounted .d64 / .d71 / .d81 (or lists the host
//   directory so one can be picked with  OPEN15,8,15,"CD:GAME.D64"  /  "CD:_" to unmount, or  LOAD"$/GAMES/GAME.D64",8).
// Uses the KERNAL bus hook from IOH_KernalReplace.c with its own image (VD_KernalImage below: /VDrive/kernal.bin on the SD card, else
// the C64's own KERNAL patched in memory; the <Shift-K> file stays the plain Kernal Replace's) and adds the IO1/IO2 mailbox the
// patched KERNAL talks to.
//
// This file is included from IOHandlers.h like the other handlers (unity build, compiled as C++).

// Optional bring-up aid: define VD_TRACE (e.g. at the top of this file) to log every mailbox command on the USB serial
// port. Off by default: the print runs right after the answer is published, i.e. while the C64 is still reading ST / DATA,
// and the USB serial interrupts then make the bus handler answer late (the C64 reads $00 / the previous bus byte).

#include <strings.h>               // strcasecmp
#define VD_COLD FLASHMEM          // everything except the bus-ISR register accessors lives in flash
#include "../vdrive/d64.c"
#include "../vdrive/vdrive.c"
#include "../vdrive/vdrive_io.c"

void VDrive_PreStart(bool agent = false);   // called from KERNALPreStart(), see StatusFunctions.c; agent: for IOH_VDriveAgent
uint8_t VDriveSwift_Remap(uint8_t ioh);   // IOH_VDriveSwift.c (IOH_VDriveAgent.c on the plain TR); used by KERNALPreStart() and IOHandlerSelectInit()
void IO2Hndlr_VDrive(uint8_t Address, bool R_Wn);
void VDrive_NoteLaunch();   // KERNALPreStart(), before anything there can free the menu's listing (below)

#ifdef Fab04_VDrive
#include "../vdrive/vd_kpatch.h"   // VD_KernalPatch[]: patches/*.s as runs (tools/build.py --table)
bool VD_KernalImage(uint8_t *img);   // KernalImageSource for InitHndlr_KERNALReplace_PreStart() (IOH_KernalReplace.c), set by KERNALPreStart()
bool VDrive_ProbeStep();  // likewise, first: the menu's bus probe (below)
void InitHndlr_VDrive();
void PollingHndlr_VDrive();
void IO1Hndlr_VDrive(uint8_t Address, bool R_Wn);

stcIOHandlers IOHndlr_VDrive =
{
  "VDrive KERNAL",          //Name of handler (IOHNameLength max)
  &InitHndlr_VDrive,        //Called once at handler startup
  &IO1Hndlr_VDrive,         //IO1 R/W handler: mailbox registers
  &IO2Hndlr_VDrive,         //IO2 R/W handler: 128-byte data window at $DF80
  NULL,                     //ROML Read handler, in addition to any ROM data sent
  NULL,                     //ROMH Read handler, in addition to any ROM data sent
  &PollingHndlr_VDrive,     //Polled in main routine: does all file system work
  NULL,                     //called at the end of EVERY c64 cycle
};
#endif

// The VDrive Special IOs: the remote control and the listings while a program runs treat them all alike. VD_IsKernalHandler: the
// ones that serve a KERNAL (PreStart loads it).
#if defined(Fab04_VDrive) && defined(Fab04_REU)
   #define VD_IsKernalHandler(h) ((h) == IOH_VDrive || (h) == IOH_VDriveSwift || (h) == IOH_VDriveSwiftREU || (h) == IOH_VDriveREU)
#elif defined(Fab04_VDrive)
   #define VD_IsKernalHandler(h) ((h) == IOH_VDrive || (h) == IOH_VDriveSwift)
#else
   #define VD_IsKernalHandler(h) false
#endif
#define VD_IsHandler(h) (VD_IsKernalHandler(h) || (h) == IOH_VDriveAgent)

#define VD_MboxBase    0x10  // mailbox at $DE10-$DE1A: $DE00-$DE07 stays free for the SwiftLink ACIA (see IOH_VDriveSwift.c)
#define VD_CwdLen      96
#define VD_PathLen     (2 * VD_CwdLen + 4)   // folder + '/' + a host name (imgName: under VD_CwdLen characters)
#define VD_FlushIdleMs 250   // write the image back this long after the last sector write

struct stcVDUnit
{
   FS      *fs;              // SD or USB file system
   File     img;             // mounted .d64
   File     dir;             // host directory being listed
   File     plain;           // plain file being read with nothing mounted (LOAD"GAME.PRG")
   d64_t    d;
   uint8_t *errTab;          // the image's error table (malloc, RAM2) when it marks any sector bad; NULL = none
   bool     mounted;
   bool     dirty;
   bool     quiet;           // temporary selection for a listing by path: leave the /.vdrive sidecar alone
   char     imgName[VD_CwdLen];   // host (ASCII) name of the mounted image, in cwd (the C64 may have used its alias)
   char     snapCwd[VD_CwdLen];   // folder / image selected before a listing by path (push / pop)
   char     snapImg[VD_CwdLen];   // "" = nothing was mounted
   uint32_t lastWriteMs;
   char     cwd[VD_CwdLen];  // "" = root
};

// The drive state (~4.6 KB, see VIO_RAM_BUDGET in tests/test_vio.py) is a plain static (RAM1/DTCM, which has room), like every
// other buffer here: never DMAMEM. RAM2's statics shrink the malloc heap the REU takes its banks from, and ours (~9.4 KB) left the
// "512KB REU" at 63 banks, rounded down to 256 KB (REUTEST, 2026-11-09). Define VD_ALLOC_HEAP to take it from the heap instead.
#ifndef VD_ALLOC_HEAP
static vio_t vioStore;
#endif
// DMA LOAD (VIO_FLOAD_DMA, docs/DESIGN.md 49), SAVE and VERIFY (VIO_FSAVE_DMA, VIO_FVERIFY_OPEN, 51), only with Fab04_VDriveDMALoad
// (Common_Defs.h; needs Fab04_FullDMACapable): one part of a file at a time, read here and then written into C64 RAM by DMA (or read
// from C64 RAM by DMA, then saved or compared). 2 KB halts the CPU for about 2.5 ms per part (IRQs and NMIs wait that long); RAM1, like
// the rest (see vioStore). Without the flag no DMA hook is set: the drive answers every LOAD part through the 128-byte window, SAVE goes
// by window blocks and VERIFY the KERNAL's stock way, the same KERNAL included.
#ifdef Fab04_VDriveDMALoad
#ifndef VD_DmaPart
#define VD_DmaPart 2048
#endif
static uint8_t vdDmaBuf[VD_DmaPart];
#endif
static vio_hot_t  vioHot;           // registers + window: the ONLY memory the bus ISR touches. Plain static = RAM1/DTCM, fast
static vio_t     *vio = NULL;       // everything else (main loop only; vioStore, or the heap with VD_ALLOC_HEAP)
static volatile bool vioReady = false;
static uint32_t   lastActivityMs = 0;
static void     (*vdDrivePoll)() = NULL;   // the running VDrive handler's drive poll (set by its init; the remote control's waits call it)
static stcVDUnit  vdUnit[2];

// ---- PETSCII <-> ASCII (names are uppercase on the C64 side) -------------------------------
FLASHMEM static char VD_ToAscii(uint8_t c)
{
   if (c >= 0x41 && c <= 0x5a) return c + 0x20;   // unshifted PETSCII letters -> lower case
   if (c >= 0xc1 && c <= 0xda) return c - 0x80;   // shifted -> upper case
   return c;
}
FLASHMEM static uint8_t VD_ToPetscii(char c) { return (c >= 'a' && c <= 'z') ? c - 0x20 : c; }

FLASHMEM static void VD_Squeeze(char *p)      // "//a///b" -> "/a/b" (the TR menu hands USB paths over with a doubled slash)
{
   char *w = p;
   for (const char *r = p; *r; r++) if (!(*r == '/' && w > p && w[-1] == '/')) *w++ = *r;
   *w = 0;
}

FLASHMEM static void VD_PathFor(stcVDUnit *u, const char *name, char *out, size_t cap)
{
   size_t n = snprintf(out, cap, "%s", u->cwd);
   while (n > 0 && n < cap && out[n - 1] == '/') n--;                 // cwd "/" must not give "//name"
   if (n + 1 < cap) out[n++] = '/';
   for (; *name && n + 1 < cap; name++) out[n++] = VD_ToAscii((uint8_t)*name);
   out[n] = 0;
}

FLASHMEM static void VD_HostPath(stcVDUnit *u, const char *host, char *out, size_t cap)   // host = ASCII name in cwd
{
   snprintf(out, cap, "%s/%s", u->cwd, host);
}

// ---- aliases: host names the C64 can't use as they are (over 16 characters, or characters it can't type) ------------------
// vd_alias() (vdrive.c) makes the C64 spelling. An alias holds only while no other entry of the folder shows the same name
// (a real name or another alias); otherwise it is dropped (not listed, not found), never numbered. Every alias holds a '^',
// so a name without one never costs a folder scan.
FLASHMEM static int VD_Shown(const char *host, char out[17])   // 0 = real name, 1 = alias, -1 = not shown
{
   if (IsDotfile(host) || strlen(host) >= VD_CwdLen) return -1;   // imgName must hold any name we mount
   return vd_alias(host, out);
}

// How many entries of the unit's folder show as `shown`; the host name of the last one goes to host (if given).
FLASHMEM static int VD_CountShown(stcVDUnit *u, const char *shown, char *host, size_t cap, int *alias)
{
   File d = u->fs->open(u->cwd[0] ? u->cwd : "/");
   if (!d) return 0;
   int n = 0;
   while (File e = d.openNextFile())
   {
      char s[17];
      int r = VD_Shown(e.name(), s);
      if (r >= 0 && strcmp(s, shown) == 0) { n++; if (host) strlcpy(host, e.name(), cap); if (alias) *alias = r; }
      e.close();
   }
   d.close();
   return n;
}

// The host path for a C64 name in the unit's folder: the name itself or, when that does not exist, the entry it is the alias of.
FLASHMEM static void VD_Resolve(stcVDUnit *u, const char *name, char *out, size_t cap)
{
   VD_PathFor(u, name, out, cap);
   if (!strchr(name, '^') || u->fs->exists(out)) return;
   char host[VD_CwdLen];
   int alias = 0;
   if (VD_CountShown(u, name, host, sizeof host, &alias) == 1 && alias == 1) VD_HostPath(u, host, out, cap);
}

// ---- d64 sector backend ---------------------------------------------------------------------
static int VD_ReadSector(void *ctx, uint8_t t, uint8_t s, uint8_t *buf)
{
   stcVDUnit *u = (stcVDUnit *)ctx;
   long off = d64_sector_offset(&u->d, t, s);
   if (off < 0 || !u->img.seek((uint32_t)off)) return -1;
   return u->img.read(buf, D64_SECTOR) == D64_SECTOR ? 0 : -1;
}

static int VD_WriteSector(void *ctx, uint8_t t, uint8_t s, const uint8_t *buf)
{
   stcVDUnit *u = (stcVDUnit *)ctx;
   long off = d64_sector_offset(&u->d, t, s);
   if (off < 0 || !u->img.seek((uint32_t)off)) return -1;
   if (u->img.write(buf, D64_SECTOR) != D64_SECTOR) return -1;
   u->dirty = true; u->lastWriteMs = millis();
   return 0;
}

FLASHMEM static void VD_Flush(stcVDUnit *u)
{
   if (u->mounted && u->dirty) { u->img.flush(); u->dirty = false; }
}

// ---- remembered mount: "/.vdrive" on each unit's own file system holds the last mounted image's path --------
// Dot-named, so the TR menu (0.9+) and our own host listing hide it. Written on every successful mount, removed on unmount.
#define VD_Sidecar "/.vdrive"

FLASHMEM static void VD_SidecarClear(stcVDUnit *u)
{
   if (u->quiet) return;
   if (u->fs->exists(VD_Sidecar)) u->fs->remove(VD_Sidecar);
}

FLASHMEM static void VD_SidecarSave(stcVDUnit *u, const char *path)
{
   if (u->quiet) return;
   VD_SidecarClear(u);                                   // FILE_WRITE appends: start from an empty file
   File f = u->fs->open(VD_Sidecar, FILE_WRITE);
   if (!f) return;
   f.write(path, strlen(path));
   f.close();
}

// ---- host services for vdrive.c (CD:, host directory listing) --------------------------------
// Bad sectors: an image with one error byte per sector after the data (175,531 / 197,376 / 351,062 / 822,400 bytes) gets the
// table copied to the heap when any byte in it is a read error, so a bad sector costs no extra card access; all-OK tables
// (the usual case) are dropped. No memory: the image mounts without it.
FLASHMEM static void VD_LoadErrTab(stcVDUnit *u, uint32_t size)
{
   u->errTab = NULL; u->d.errtab = NULL;
   long off = d64_errtab_offset(&u->d, size);
   if (off < 0 || !u->img.seek((uint32_t)off)) return;
   int n = d64_sectors(&u->d), nbad = 0;
   uint8_t *t = (uint8_t *)malloc(n);
   if (!t) return;
   if (u->img.read(t, n) != n) { free(t); return; }
   for (int i = 0; i < n; i++) if (d64_errcode(t[i])) nbad++;
   if (!nbad) { free(t); return; }
   u->errTab = t; u->d.errtab = t;
   Printf_dbg("VDrive: error table, %d bad sectors\n", nbad);
}

FLASHMEM static void VD_Unmount(void *ctx, int unit)
{
   stcVDUnit *u = &vdUnit[unit];
   VD_Flush(u);
   if (u->mounted) u->img.close();
   u->mounted = false;
   free(u->errTab); u->errTab = NULL; u->d.errtab = NULL;
   VD_SidecarClear(u);                                   // an explicit unmount forgets the image (a new mount re-writes it)
}

// Mount the image at path (host spelling); imgName keeps its host name.
FLASHMEM static int VD_MountHost(int unit, const char *path, d64_t **out)
{
   stcVDUnit *u = &vdUnit[unit];
   const char *slash = strrchr(path, '/');
   const char *leaf = slash ? slash + 1 : path;
   if (strlen(leaf) >= sizeof u->imgName) return 33;
   VD_Unmount(NULL, unit);
   // Check existence and size with a read-only open first: FILE_WRITE creates missing files on SD/USB, and a mistyped
   // CD: must never litter the card with empty "images".
   File probe = u->fs->open(path, FILE_READ);
   if (!probe || probe.isDirectory()) { if (probe) probe.close(); return 62; }   // FILE NOT FOUND
   uint32_t size = probe.size();
   probe.close();
   if (size < 174848) return 74;                                                  // DRIVE NOT READY: not a D64
   bool writable = true;
   u->img = u->fs->open(path, FILE_WRITE);                                        // exists now, so nothing is created
   if (!u->img) { writable = false; u->img = u->fs->open(path, FILE_READ); }
   if (!u->img) return 62;
   if (d64_open_size(&u->d, VD_ReadSector, u, size)) { u->img.close(); return 74; }   // format (d64 / d71 / d81) comes from the size
   u->d.write = writable ? VD_WriteSector : NULL;
   VD_LoadErrTab(u, size);
   u->mounted = true; u->dirty = false;
   strlcpy(u->imgName, leaf, sizeof u->imgName);
   *out = &u->d;
   VD_SidecarSave(u, path);
   Printf_dbg("VDrive %d: mounted %s%s\n", unit + 8, path, writable ? "" : " (read-only)");
   return 0;
}

FLASHMEM static int VD_Mount(void *ctx, int unit, const char *name, d64_t **out)   // name = C64 (PETSCII) name or alias
{
   char path[VD_PathLen];
   VD_Resolve(&vdUnit[unit], name, path, sizeof path);
   return VD_MountHost(unit, path, out);
}

// "N:NAME.D64,ID" with nothing mounted: a new all-zero image file in the current folder, then mounted (vdrive.c formats it).
// Written in 4 KB pieces: a D81 is 800 KB and the whole command must finish inside the KERNAL's ~1.6 s mailbox wait.
FLASHMEM static int VD_Create(void *ctx, int unit, const char *name, uint32_t size, d64_t **out)
{
   stcVDUnit *u = &vdUnit[unit];
   char path[VD_PathLen];
   VD_Resolve(u, name, path, sizeof path);
   if (u->fs->exists(path)) return 63;                                            // FILE EXISTS (or an alias of one): never replaced
   File f = u->fs->open(path, FILE_WRITE);
   if (!f) return 26;                                                             // card write-protected / not writable
   static const uint32_t chunk = 4096;
   uint8_t *zero = (uint8_t *)calloc(1, chunk);
   bool ok = zero != NULL;
   for (uint32_t n = 0; ok && n < size; n += chunk) {
      uint32_t k = size - n < chunk ? size - n : chunk;
      ok = f.write(zero, k) == k;
   }
   free(zero);
   f.close();
   if (!ok) { u->fs->remove(path); return 72; }                                   // DISK FULL (the card)
   Printf_dbg("VDrive %d: created %s (%lu bytes)\n", unit + 8, path, (unsigned long)size);
   return VD_MountHost(unit, path, out);
}

// Plain files in the current folder, read with nothing mounted: LOAD"GAME.PRG" (vdrive.c tries NAME.PRG and patterns itself).
FLASHMEM static void VD_PlainClose(void *ctx, int unit)
{
   stcVDUnit *u = &vdUnit[unit];
   if (u->plain) u->plain.close();
}

FLASHMEM static int VD_PlainOpen(void *ctx, int unit, const char *name, uint32_t *size)
{
   stcVDUnit *u = &vdUnit[unit];
   char path[VD_PathLen];
   VD_Resolve(u, name, path, sizeof path);
   VD_PlainClose(ctx, unit);
   u->plain = u->fs->open(path, FILE_READ);                                       // FILE_READ: a missing file is not created
   if (!u->plain) return 62;
   if (u->plain.isDirectory()) { u->plain.close(); return 62; }
   *size = u->plain.size();
   return 0;
}

FLASHMEM static int VD_PlainRead(void *ctx, int unit, uint8_t *buf, uint16_t n)
{
   stcVDUnit *u = &vdUnit[unit];
   return u->plain ? u->plain.read(buf, n) : -1;
}

// MD:NAME / RD:NAME with nothing mounted: folders on the card
FLASHMEM static int VD_MkDir(void *ctx, int unit, const char *name)
{
   stcVDUnit *u = &vdUnit[unit];
   char path[VD_PathLen];
   VD_Resolve(u, name, path, sizeof path);
   if (u->fs->exists(path)) return 63;                                            // FILE EXISTS (a file or a folder, or an alias of one)
   return u->fs->mkdir(path) ? 0 : 26;
}

FLASHMEM static int VD_RmDir(void *ctx, int unit, const char *name)
{
   stcVDUnit *u = &vdUnit[unit];
   char path[VD_PathLen];
   VD_Resolve(u, name, path, sizeof path);
   File d = u->fs->open(path, FILE_READ);
   if (!d || !d.isDirectory()) { if (d) d.close(); return 62; }
   File first = d.openNextFile();
   bool empty = !first;
   if (first) first.close();
   d.close();
   if (!empty) return 63;                                                         // not empty: nothing is removed
   return u->fs->rmdir(path) ? 0 : 26;
}

// S:NAME.D64 with nothing mounted: delete an image file (vdrive.c checks the name). A file with the FAT read-only attribute
// cannot be opened for writing (SdFat), so it stays: 26 WRITE PROTECT ON.
FLASHMEM static int VD_Scratch(void *ctx, int unit, const char *name)
{
   stcVDUnit *u = &vdUnit[unit];
   char path[VD_PathLen];
   VD_Resolve(u, name, path, sizeof path);
   File probe = u->fs->open(path, FILE_READ);                                     // FILE_READ first: FILE_WRITE would create it
   if (!probe || probe.isDirectory()) { if (probe) probe.close(); return 62; }
   probe.close();
   File w = u->fs->open(path, FILE_WRITE);
   if (!w) return 26;
   w.close();
   if (!u->fs->remove(path)) return 26;
   Printf_dbg("VDrive %d: scratched %s\n", unit + 8, path);
   return 0;
}

// A listing by path (LOAD"$/GAMES/G.D64") must not change what is selected: remember folder and image, put them back afterwards.
FLASHMEM static void VD_Push(void *ctx, int unit)
{
   stcVDUnit *u = &vdUnit[unit];
   strlcpy(u->snapCwd, u->cwd, sizeof u->snapCwd);
   strlcpy(u->snapImg, u->mounted ? u->imgName : "", sizeof u->snapImg);
   u->quiet = true;                                       // the sidecar keeps naming the real selection meanwhile
}

FLASHMEM static int VD_Pop(void *ctx, int unit, d64_t **out)
{
   stcVDUnit *u = &vdUnit[unit];
   if (!out)                                              // keep what the walk selected (a file is being loaded through the path)
   {
      u->quiet = false;
      if (u->mounted)
      {
         char path[VD_PathLen];
         VD_HostPath(u, u->imgName, path, sizeof path);
         VD_SidecarSave(u, path);
      }
      else VD_SidecarClear(u);
      return 0;
   }
   VD_Unmount(ctx, unit);
   strlcpy(u->cwd, u->snapCwd, sizeof u->cwd);
   *out = NULL;
   int r = 0;
   if (u->snapImg[0])
   {
      char path[VD_PathLen];
      VD_HostPath(u, u->snapImg, path, sizeof path);
      r = VD_MountHost(unit, path, out);
   }
   if (r) *out = NULL;
   u->quiet = false;
   return r;
}

FLASHMEM static int VD_ChDir(void *ctx, int unit, const char *name)
{
   stcVDUnit *u = &vdUnit[unit];
   if (strcmp(name, "/") == 0) { u->cwd[0] = 0; return 0; }   // card root
   if (strcmp(name, "..") == 0 || strcmp(name, "_") == 0)
   {
      char *slash = strrchr(u->cwd, '/');
      if (slash) *slash = 0; else u->cwd[0] = 0;
      return 0;
   }
   char path[VD_PathLen];
   VD_Resolve(u, name, path, sizeof path);
   size_t l = strlen(path); if (l > 1 && path[l - 1] == '/') path[l - 1] = 0;   // drop trailing '/'
   File d = u->fs->open(path);
   if (!d || !d.isDirectory()) { if (d) d.close(); return 62; }
   d.close();
   if (strlen(path) >= VD_CwdLen) return 74;
   strcpy(u->cwd, path);
   return 0;
}

// Hash bits of the names a folder shows, per unit: a first pass in VD_DirOpen marks names seen twice, so the listing counts
// an alias's twins (a folder scan) only when its bit says there may be one.
static uint8_t vdSeen[2][128], vdDup[2][128];
FLASHMEM static unsigned VD_NameHash(const char *s)
{
   uint32_t h = 2166136261u;
   while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
   return h & 1023;
}

FLASHMEM static int VD_DirOpen(void *ctx, int unit, char title[17])
{
   stcVDUnit *u = &vdUnit[unit];
   if (u->dir) u->dir.close();
   memset(vdSeen[unit], 0, sizeof vdSeen[unit]); memset(vdDup[unit], 0, sizeof vdDup[unit]);
   File d = u->fs->open(u->cwd[0] ? u->cwd : "/");
   while (d) {
      File e = d.openNextFile();
      if (!e) break;
      char s[17];
      if (VD_Shown(e.name(), s) >= 0)
      {
         unsigned h = VD_NameHash(s); uint8_t b = (uint8_t)(1 << (h & 7));
         if (vdSeen[unit][h >> 3] & b) vdDup[unit][h >> 3] |= b;
         vdSeen[unit][h >> 3] |= b;
      }
      e.close();
   }
   if (d) d.close();
   u->dir = u->fs->open(u->cwd[0] ? u->cwd : "/");
   if (!u->dir) return 1;
   snprintf(title, 17, "%s:%s", unit == 0 ? "SD" : "USB", u->cwd[0] ? u->cwd : "/");
   for (char *p = title; *p; p++) *p = VD_ToPetscii(*p);
   return 0;
}

FLASHMEM static int VD_DirNext(void *ctx, int unit, char name[17], uint32_t *size, int *is_dir)
{
   stcVDUnit *u = &vdUnit[unit];
   while (File e = u->dir.openNextFile())
   {
      int r = VD_Shown(e.name(), name);                       // upper-case ASCII = PETSCII
      if (r == 1)
      {
         unsigned h = VD_NameHash(name);
         if ((vdDup[unit][h >> 3] & (1 << (h & 7))) && VD_CountShown(u, name, NULL, 0, NULL) > 1) r = -1;   // a twin: no alias
      }
      if (r < 0) { e.close(); continue; }
      *is_dir = e.isDirectory();
      *size = *is_dir ? 0 : e.size();
      e.close();
      return 1;
   }
   u->dir.close();
   return 0;
}

// ---- host text files for the configuration utility (VDrive Config.prg): OPEN n,d,n,"!CONFIG[,W]" and "!STATE" ---------------------
// !CONFIG is the unit's /vdrive-config.txt (see the Alt button section below), !STATE three lines: the mounted image's path, the
// folder and the TR menu's colour references (IO1[rwRegColorRefStart..], NumColorRefs hex digits, so the utility matches the menu).
// Text crosses as PETSCII with CR line ends. One file at a time; opening another (or closing the channel) ends the first.
#define VD_ConfigName "/vdrive-config.txt"
extern volatile uint8_t* IO1;     // defined in IOH_TeensyROM.c, which is included after this file
static File       hfFile;
static int        hfKind = -1;
static bool       hfWriting = false;
static char       hfText[VD_PathLen + VD_CwdLen + NumColorRefs + 6];
static size_t     hfTextPos = 0, hfTextLen = 0;

FLASHMEM static void VD_HfClose(void *ctx, int unit, int write)
{
   if (hfFile) { if (write) hfFile.flush(); hfFile.close(); }
   hfKind = -1; hfWriting = false;
}

FLASHMEM static int VD_HfOpen(void *ctx, int unit, int kind, int write)
{
   VD_HfClose(NULL, unit, 0);
   stcVDUnit *u = &vdUnit[unit];
   if (kind == VD_HF_STATE)
   {
      char path[VD_PathLen];
      path[0] = 0;
      if (u->mounted) VD_HostPath(u, u->imgName, path, sizeof path);
      hfTextLen = snprintf(hfText, sizeof hfText, "%s\n%s\n", path, u->cwd[0] ? u->cwd : "/");
      if (hfTextLen > sizeof hfText - NumColorRefs - 2) hfTextLen = sizeof hfText - NumColorRefs - 2;
      for (int i = 0; i < NumColorRefs; i++) hfText[hfTextLen++] = "0123456789ABCDEF"[IO1[rwRegColorRefStart + i] & 15];
      hfText[hfTextLen++] = '\n';
      hfText[hfTextLen] = 0;
      hfTextPos = 0; hfKind = kind;
      return 0;
   }
   if (write)
   {
      if (u->fs->exists(VD_ConfigName)) u->fs->remove(VD_ConfigName);   // FILE_WRITE appends: start from an empty file
      hfFile = u->fs->open(VD_ConfigName, FILE_WRITE);
      if (!hfFile) return 74;
   }
   else
   {
      hfFile = u->fs->open(VD_ConfigName, FILE_READ);
      if (!hfFile || hfFile.isDirectory()) { if (hfFile) hfFile.close(); return 62; }
   }
   hfKind = kind; hfWriting = write != 0;
   return 0;
}

FLASHMEM static int VD_HfRead(void *ctx, int unit, uint8_t *buf, int n)
{
   int got = 0;
   if (hfKind == VD_HF_STATE)
   {
      for (; got < n && hfTextPos < hfTextLen; got++, hfTextPos++)
         buf[got] = hfText[hfTextPos] == '\n' ? '\r' : VD_ToPetscii(hfText[hfTextPos]);
      return got;
   }
   if (hfKind != VD_HF_CONFIG || !hfFile || hfWriting) return 0;
   while (got < n)
   {
      uint8_t c;
      if (hfFile.read(&c, 1) != 1) break;
      if (c == '\r') continue;                                          // CRLF files: the CR of the pair is dropped
      buf[got++] = c == '\n' ? '\r' : VD_ToPetscii((char)c);
   }
   return got;
}

FLASHMEM static int VD_HfWrite(void *ctx, int unit, const uint8_t *buf, int n)
{
   if (hfKind != VD_HF_CONFIG || !hfFile || !hfWriting) return 1;
   for (int i = 0; i < n; i++)
   {
      char c = buf[i] == '\r' ? '\n' : VD_ToAscii(buf[i]);
      if (buf[i] == '\n') continue;                                      // a stray LF (PRINT# to device 10 / 13) adds nothing
      if (hfFile.write(&c, 1) != 1) return 1;
   }
   return 0;
}

// ---- fixed device numbers in EEPROM (0 / out of range = automatic, see vdrive_set_devices) --------------------
FLASHMEM static void VD_SaveDevices(void *ctx, uint8_t pin0, uint8_t pin1)
{
   EEPROM.write(eepAdVDriveDevs, pin0);        // written by "U0>"+CHR$(n) on the drive's command channel
   EEPROM.write(eepAdVDriveDevs + 1, pin1);
}

FLASHMEM static void VD_LoadDevices()
{
   vdrive_set_devices(&vio->vd, EEPROM.read(eepAdVDriveDevs), EEPROM.read(eepAdVDriveDevs + 1));   // validates: unset / garbage / duplicate = automatic
}

// ---- mount at start-up: the image opened in the TeensyROM menu, else the one remembered in the sidecar ------------------
// Opening a .d64 in the menu makes DriveDirPath "<dir>/<image>.d64*" (trailing '*' = inside an image). When a program is
// launched from there, that image is mounted on the unit of the same storage (SD = unit 0, USB = unit 1) so the program's own
// LOAD from device 8/9 finds its files. Any other launch re-mounts what the unit had mounted last.
extern char DriveDirPath[];
extern volatile uint8_t BtnPressed;   // Teensy.ino: the menu button; loop() then resets the C64 into the TR menu

// path = "/a/b/X.D64" (ASCII, no trailing '*'): folder becomes the unit's cwd, then the image is mounted.
FLASHMEM static int VD_MountPath(int unit, const char *rawPath)   // 0 ok, else a DOS error code
{
   stcVDUnit *u = &vdUnit[unit];
   char sq[VD_PathLen];
   if (strlen(rawPath) >= sizeof sq) return 33;
   strlcpy(sq, rawPath, sizeof sq);
   VD_Squeeze(sq);
   const char *path = sq;
   const char *slash = strrchr(path, '/');
   const char *file = slash ? slash + 1 : path;
   size_t dl = slash ? (size_t)(slash - path) : 0;                 // "/a/b/X.D64" -> cwd "/a/b"; root -> ""
   if (dl >= VD_CwdLen || !*file) return 33;
   memcpy(u->cwd, path, dl); u->cwd[dl] = 0;
   d64_t *d = NULL;
   int r = VD_MountHost(unit, path, &d);
   if (r == 0) vdrive_attach(&vio->vd, unit, d, 1);
   else u->cwd[0] = 0;
   return r;
}

// The menu source and DriveDirPath of this launch, noted by VDrive_NoteLaunch() before KERNALPreStart() frees the menu's listing
// for an REU Special IO (docs/DESIGN.md 50): upstream's FreeDriveDirMenu() turns the menu back to the Teensy one and DriveDirPath to
// "/", so VDrive_PreStart() read "not from an image" and mounted the remembered one (0.9.0.55-0.9.0.57).
static uint8_t vdLaunchMenu = 0xFF;          // 0xFF: nothing noted, PreStart reads the live values
static char vdLaunchPath[VD_PathLen];

FLASHMEM void VDrive_NoteLaunch()
{
   vdLaunchMenu = IO1[rWRegCurrMenuWAIT];
   size_t n = strlen(DriveDirPath);
   if (n < sizeof vdLaunchPath) memcpy(vdLaunchPath, DriveDirPath, n + 1);
   else vdLaunchPath[0] = 0;                 // too long to mount anyway (VD_StartupMount)
}

FLASHMEM static void VD_StartupMount(int unit, bool menuUnit, const char *dirPath)
{
   stcVDUnit *u = &vdUnit[unit];
   if (!vio->vd.unit[unit].present) return;
   char path[VD_PathLen];
   if (menuUnit)
   {
      size_t n = strlen(dirPath);
      if (n >= 2 && dirPath[n - 1] == '*' && n < sizeof path)
      {
         memcpy(path, dirPath, n - 1); path[n - 1] = 0;
         VD_MountPath(unit, path);
         return;
      }
   }
   File f = u->fs->open(VD_Sidecar, FILE_READ);
   if (!f) return;
   int n = f.read(path, sizeof path - 1);
   f.close();
   if (n <= 0) return;
   path[n] = 0;
   VD_MountPath(unit, path);
}

// ---- handler ---------------------------------------------------------------------------------
extern bool SDFullInit();
extern bool USBFileSystemWait();
#ifdef Fab04_VDrive
void InitHndlr_KernalReplace();   // re-stages the KERNAL bus hook (IOH_KernalReplace.c)
#endif

// All storage access (SD/USB probing, sizing) happens here, called from KERNALPreStart() while the C64 is still
// waiting for the TR ("WaitForTRDots"). Doing it later is unsafe: SD reads mask interrupts, and once the replacement
// KERNAL is live every C64 instruction fetch from $E000-$FFFF depends on the bus ISR running.
#ifdef Fab04_VDrive
FLASHMEM static void VD_ProbeBegin();
void VD_BannerUnits(uint8_t *img, int size, bool settled);   // below
void VD_BannerRegs(bool on, bool settled);

#ifdef Fab04_VDriveDMALoad
// vio's DMA write: the CPU waits in the KERNAL's mailbox loop meanwhile (vcall), and the bus ISR skips the snoop for the transfer.
FLASHMEM static int VD_DmaWrite(void *, uint16_t addr, const uint8_t *buf, uint16_t len)
{
   return PerformDMA(DMA_WRITE, addr, (uint8_t *)buf, len, DMA_ADDR_INCREMENT) && CloseDMA();
}
// vio's DMA read (SAVE, VERIFY, and the KERNAL's pointers in zero page): only $0000-$CFFF is asked for (vio_dma_rd_ok and the
// VIO_DMA_* zero-page bytes), where a DMA read sees what the CPU sees; never the served KERNAL's $E000-$FFFF.
FLASHMEM static int VD_DmaRead(void *, uint16_t addr, uint8_t *buf, uint16_t len)
{
   return PerformDMA(DMA_READ, addr, buf, len, DMA_ADDR_INCREMENT) && CloseDMA();
}
#endif
#endif

static bool vdNoMenuProbe = false;  // VD_KernalImage: the C64 keeps a KERNAL that is not this build's VDrive one (see there); one PreStart

FLASHMEM void VDrive_PreStart(bool agent)
{
#ifdef VD_ALLOC_HEAP
   if (vio == NULL) vio = (vio_t *)malloc(sizeof(vio_t));
#else
   vio = &vioStore;                 // vio_init() clears it on every start
#endif
   if (vio == NULL) { Serial.println("VDrive: out of memory"); return; }
   vioReady = false;
   vio_init(vio, &vioHot);
#ifdef Fab04_VDriveDMALoad
   vio_set_dma(vio, VD_DmaWrite, VD_DmaRead, NULL, vdDmaBuf, sizeof vdDmaBuf);
#endif

   vd_host_t host;
   memset(&host, 0, sizeof host);
   host.mount = VD_Mount;  host.unmount = VD_Unmount;  host.chdir = VD_ChDir;
   host.dir_open = VD_DirOpen;  host.dir_next = VD_DirNext;  host.save_devices = VD_SaveDevices;
   host.push = VD_Push;  host.pop = VD_Pop;  host.create = VD_Create;
   host.hf_open = VD_HfOpen;  host.hf_read = VD_HfRead;  host.hf_write = VD_HfWrite;  host.hf_close = VD_HfClose;
   host.plain_open = VD_PlainOpen;  host.plain_read = VD_PlainRead;  host.plain_close = VD_PlainClose;
   host.mkdir = VD_MkDir;  host.rmdir = VD_RmDir;  host.scratch = VD_Scratch;
   if (hfFile) hfFile.close();        // the drive state (and with it any open channel) was just reset
   hfKind = -1;
   vdrive_set_host(&vio->vd, &host);
   VD_LoadDevices();

   for (int i = 0; i < 2; i++) { vdUnit[i].mounted = false; vdUnit[i].dirty = false; vdUnit[i].quiet = false; vdUnit[i].cwd[0] = 0;
                                 free(vdUnit[i].errTab); vdUnit[i].errTab = NULL; }
   vdUnit[0].fs = &SD;
   vdUnit[1].fs = &firstPartition;
   vdrive_attach(&vio->vd, 0, NULL, SDFullInit());
   vdrive_attach(&vio->vd, 1, NULL, USBFileSystemWait());
   uint8_t menu = vdLaunchMenu != 0xFF ? vdLaunchMenu : IO1[rWRegCurrMenuWAIT];
   const char *dirPath = vdLaunchMenu != 0xFF ? vdLaunchPath : DriveDirPath;
   VD_StartupMount(0, menu == rmtSD, dirPath);
   VD_StartupMount(1, menu == rmtUSBDrive, dirPath);
   vdLaunchMenu = 0xFF;             // one launch only
   int scanPending = vio_arm_scan(vio);   // automatic numbers: probed by the menu next (else on the first disk access, by the KERNAL
   vio_publish_devices(vio);              //   or, with the agent, by the vector engine: docs/DESIGN.md 53)
#ifdef Fab04_VDrive
   if (!agent) {
      VD_BannerUnits(KernalFast, 8192, false);   // the image InitHndlr_KERNALReplace_PreStart() just loaded, before the C64 prints its banner
      VD_BannerRegs(true, false);                // and the same for a VDrive KERNAL the C64 holds itself
   }
#endif
   vioReady = true;                 // only now may the bus handlers answer
   Serial.printf("VDrive: dev %d (SD) %s, dev %d (USB) %s%s\n",
                 vio->vd.dev[0], vio->vd.unit[0].present ? "ready" : "absent",
                 vio->vd.dev[1], vio->vd.unit[1].present ? "ready" : "absent",
                 !scanPending ? "" : agent ? " (numbers are settled on the first disk access)"
                 : !vdNoMenuProbe ? " (numbers are settled by the bus probe)"
                 : " (no bus probe in the menu: the C64's own KERNAL is not this build's; the KERNAL scans on its first disk access)");
#ifdef Fab04_VDrive
   if (scanPending && !agent && !vdNoMenuProbe) VD_ProbeBegin();   // the agent leaves rwRegScratch at 0: the menu skips its probe, the engine scans
#endif
   vdNoMenuProbe = false;
}

#ifdef Fab04_VDrive
// ---- the menu's bus probe before launch (MainMenu.asm, VDriveProbe; docs/DESIGN.md 37) -------------------------------------
// Only the 6502 can see which numbers real drives hold. The TR menu probes the bus right after this PreStart, so DEV0/DEV1
// are right from the program's first instruction (the KERNAL's own scan, on the first disk access, is the fallback and is
// not asked for after this one). It probes with the C64's own KERNAL: the replacement is held back until the probe ends,
// since its serial hooks would talk to the mailbox, which is not live while the menu runs.
// The menu clears rwRegScratch before PreStart; a nonzero answer there asks for the probe. For each number from 8 up that
// no drive answers, the menu writes it to rwRegScratch and runs PreStart again; the answer is how many units still wait
// (0: done). VD_ProbeEnd (0xff: the menu ran out of numbers) also ends it. Every end hooks the replacement KERNAL back in.
#define VD_ProbeEnd 0xff
static bool vdProbing = false;
static bool (*vdProbeSnoop)(uint16_t Address, bool R_Wn) = NULL;

FLASHMEM static void VD_ProbeBegin()
{
   int need = vio_probe_begin(vio);
   IO1[rwRegScratch] = (uint8_t)need;
   if (!need) return;
   vdProbeSnoop = fBusSnoop;        // the KERNAL check InitHndlr_KERNALReplace_PreStart() just hooked (NULL if no image)
   fBusSnoop = NULL;
   vdProbing = true;
}

FLASHMEM static void VD_ProbeFinish()
{
   vdProbing = false;
   KDS_State = KDS_Idle;            // the C128's DMA HIRAM sense: the check was unhooked, no sense can be in flight
   KDS_Read01Valid = false;
   fBusSnoop = vdProbeSnoop ? KernalCheckArm() : NULL;   // the C64's or the C128's check, with HIRAM_State set for it
   VD_BannerUnits(KernalFast, 8192, true);   // the probed numbers, so the banner need not probe again
   VD_BannerRegs(true, true);
   Serial.printf("VDrive: bus probe gave dev %d (SD), %d (USB)\n", vio->vd.dev[0], vio->vd.dev[1]);
}

// true: this PreStart call was a probe step, answered in rwRegScratch; false: an ordinary PreStart (rwRegScratch = 0)
FLASHMEM bool VDrive_ProbeStep()
{
   uint8_t dev = IO1[rwRegScratch];
   if (!vdProbing || dev == 0)                // a new launch (any Special IO): a probe left over from a reset is dropped,
   {                                         // and so are the banner registers of an earlier VDrive launch
      vdProbing = false;
      VD_BannerRegs(false, false);
      return false;
   }
   int left = (dev == VD_ProbeEnd) ? 0 : vio_probe_free(vio, dev);
   if (!left) VD_ProbeFinish();
   IO1[rwRegScratch] = (uint8_t)left;
   return true;
}

// The power-on banner's second line ends in "VDRIVE SD 8 USB 9" (patches/vdbanner.s). The C64 prints it before this handler owns the
// mailbox, so the numbers travel in the KERNAL image being served: the two bytes after the tag "VDUNITS:" (not printed) say per unit
// (SD, USB) $FE = absent or no number ("NOSD" / "NOUSB"), 8-30 = the number, $FF = automatic and not settled yet; 0 / 0, as built,
// means "not written" and prints the stock line. PreStart writes the pinned numbers and $FF for automatic units; when the menu's bus
// probe finishes (settled), the numbers it gave replace the $FF. Without that probe (another launch path, or a menu without it) the
// KERNAL probes the real bus for the $FF units itself, the way the run's first disk access will (bus_scan), so the banner shows the
// numbers that scan is going to give. An image without the tag (an older kernal-tr.bin, another KERNAL) is left alone. Runs while
// the C64 still waits in the TR menu, so nothing reads them yet.
#define VD_BannerRegBase 60   // IO1 offset of the banner registers under the TR menu's handler ($DE3C)
static_assert(VD_BannerRegBase + 4 <= IO1Size, "the banner registers must lie inside the menu's IO1 array");
static const uint8_t VD_BannerTag[8] = { 'V', 'D', 'U', 'N', 'I', 'T', 'S', ':' };

FLASHMEM static bool VD_HasBannerTag(const uint8_t *img)   // in an 8 KB image: a VDrive KERNAL from 0.9.0.36 on
{
   for (int a = 0; a + (int)sizeof VD_BannerTag <= 8192; a++)
      if (memcmp(img + a, VD_BannerTag, sizeof VD_BannerTag) == 0) return true;
   return false;
}

FLASHMEM static uint8_t VD_BannerByte(int i, bool settled)
{
   uint8_t d = vio->vd.dev[i];
   return !vio->vd.unit[i].present ? 0xFE : !vio->vd.pin[i] && !settled ? 0xFF : d ? d : 0xFE;
}

FLASHMEM void VD_BannerUnits(uint8_t *img, int size, bool settled)
{
   for (int a = 0; a + (int)sizeof VD_BannerTag + 2 <= size; a++)
      if (memcmp(img + a, VD_BannerTag, sizeof VD_BannerTag) == 0)
      {
         for (int i = 0; i < 2; i++) img[a + sizeof VD_BannerTag + i] = VD_BannerByte(i, settled);
         return;
      }
}

// The same two bytes for a VDrive KERNAL the C64 holds itself (an EPROM, a MEGA65 core: docs/DESIGN.md 46), which the firmware
// cannot write into. The menu prints the banner itself after PreStart (MainMenu.asm, XferCopyRun: the KERNAL's init routines and
// INITMS, no reset) while the menu's own handler still answers IO1, and it answers a plain read with IO1[n]. Offsets 60-63
// ($DE3C-$DE3F) lie inside IO1Size between rwRegPwrUpDefaults3 (52) and StartSIDRegs (64), used by nothing: the tag $D6 $64, then SD,
// USB. Cleared by every other PreStart (VDrive_ProbeStep), so a launch with another Special IO never shows a stale VDRIVE field.
FLASHMEM void VD_BannerRegs(bool on, bool settled)
{
   IO1[VD_BannerRegBase + 0] = on ? 0xD6 : 0;
   IO1[VD_BannerRegBase + 1] = on ? 0x64 : 0;
   IO1[VD_BannerRegBase + 2] = on ? VD_BannerByte(0, settled) : 0;
   IO1[VD_BannerRegBase + 3] = on ? VD_BannerByte(1, settled) : 0;
}

// ---- the KERNAL image (docs/DESIGN.md 40) ---------------------------------------------------------------------------------
// The VDrive Special IOs serve their own image, not the <Shift-K> file (that one stays the plain Kernal Replace's, so another ROM
// there does not take the drives away): /VDrive/kernal.bin on the SD card when it is there (8192 bytes, served as it is: a
// kernal-tr.bin built on a PC from any base ROM), else the C64's own KERNAL, read by DMA while the menu waits in PreStart and
// patched here with VD_KernalPatch. Only ROMs the patch was tested on are patched (VD_KnownKernals, by CRC-32): our code calls into
// the stock routines, so a ROM that differs there (JiffyDOS, ...) would crash. Anything else, or a failed read, serves nothing:
// the C64 runs its own KERNAL and the drives stay silent (the serial console says why).
#define VD_KernalFile "/VDrive/kernal.bin"
struct stcVDKnownKernal { uint32_t crc; const char *name; };
static const stcVDKnownKernal VD_KnownKernals[] = {   // each checked in VICE with tools/vdrive_mock.py ... variants
   { 0xdbe3e7c7, "901227-03" },                       // C64 (also the C128's C64 mode on some boards)
   { 0xa5c687b3, "901227-02" },                       // early C64
   { 0x2c5965d4, "251104-04 (SX-64)" },
   { 0xdd2976d2, "325179-01 (C128 C64 mode)" },
   { 0xf10c2c25, "325182-01 (C128DCR C64 mode)" },
};

FLASHMEM uint32_t VD_Crc32(const uint8_t *p, size_t n)   // the zip / zlib CRC-32
{
   uint32_t c = 0xffffffff;
   while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320 & (0 - (c & 1))); }
   return ~c;
}

// true: every run of VD_KernalPatch is in img already, byte for byte: the C64 holds this build's VDrive KERNAL itself (burnt into a
// ROM in the socket, a MEGA65 C64 core's KERNAL file). A table run outside $E000-$FFFF: false.
FLASHMEM bool VD_KernalPatchPresent(const uint8_t *img)
{
   for (const uint8_t *r = VD_KernalPatch; ; )
   {
      uint16_t addr = (r[0] << 8) | r[1], len = (r[2] << 8) | r[3];
      if (!len) return true;
      if (addr < 0xE000 || addr + len > 0x10000 || memcmp(img + (addr - 0xE000), r + 4, len) != 0) return false;
      r += 4 + len;
   }
}

// Lays VD_KernalPatch over an 8 KB image at $E000; false (image untouched) if a run would leave it, which would be a build error.
FLASHMEM bool VD_KernalPatchApply(uint8_t *img)
{
   for (int pass = 0; pass < 2; pass++)              // check every run first, then write
      for (const uint8_t *r = VD_KernalPatch; ; )
      {
         uint16_t addr = (r[0] << 8) | r[1], len = (r[2] << 8) | r[3];
         if (!len) break;
         if (addr < 0xE000 || addr + len > 0x10000) return false;
         if (pass) memcpy(img + (addr - 0xE000), r + 4, len);
         r += 4 + len;
      }
   return true;
}

// The image the C64 holds at $E000-$FFFF: in the menu HIRAM is set and nothing answers ROMH reads there, so a DMA read sees the chip.
FLASHMEM static bool VD_KernalRead(uint8_t *img)
{
   for (uint32_t off = 0; off < 8192; off += 1024)   // in pieces, well inside DMATransferCeilingmS each
      if (!PerformDMA(DMA_READ, 0xE000 + off, img + off, 1024, DMA_ADDR_INCREMENT) || !CloseDMA()) return false;
   return true;
}

// Nothing served (false) leaves the mailbox live all the same: a VDrive KERNAL the C64 holds itself still has its drives. Only this
// build's one gets the menu's bus probe (vdNoMenuProbe): the probe runs through the C64's own KERNAL under the menu's IO1 handler,
// and a VDrive KERNAL older than the mailbox gate (vd_here in patches/vdrive.s) would write the menu's EEPROM-backed registers
// at $DE11 / $DE15 there (MIDI settings, timezone). Without the probe the KERNAL's own scan settles the numbers.
FLASHMEM bool VD_KernalImage(uint8_t *img)
{
   vdNoMenuProbe = true;
   if (SDFullInit())
   {
      File f = SD.open(VD_KernalFile, FILE_READ);
      if (f)
      {
         bool ok = !f.isDirectory() && f.size() == 8192 && f.read(img, 8192) == 8192;
         f.close();
         Serial.printf("VDrive: KERNAL %s%s\n", VD_KernalFile, ok ? "" : ": not 8192 bytes, not used");
         if (ok) { vdNoMenuProbe = false; return true; }
      }
   }
   if (!VD_KernalRead(img)) { Serial.println("VDrive: could not read the C64's KERNAL (DMA), drives off"); return false; }
   uint32_t crc = VD_Crc32(img, 8192);
   for (const stcVDKnownKernal &k : VD_KnownKernals)
      if (k.crc == crc)
      {
         if (!VD_KernalPatchApply(img)) { Serial.println("VDrive: bad KERNAL patch table, drives off"); return false; }
         Serial.printf("VDrive: the C64's KERNAL %s, patched (%d bytes)\n", k.name, VD_KernalPatchBytes);
         vdNoMenuProbe = false;
         return true;
      }
   if (VD_KernalPatchPresent(img))
   {
      Serial.println("VDrive: the C64's own KERNAL is this build's VDrive KERNAL (a ROM, a MEGA65 core): nothing served, the drives answer it");
      vdNoMenuProbe = false;
      return false;
   }
   if (VD_HasBannerTag(img))
   {
      Serial.printf("VDrive: the C64's own KERNAL is another build's VDrive KERNAL (CRC-32 %08lx): nothing served, the drives answer it, "
                    "no bus probe in the menu; rebuild it from this revision\n", (unsigned long)crc);
      return false;
   }
   Serial.printf("VDrive: the C64's KERNAL (CRC-32 %08lx) is not one the patch knows, drives off: put a patched image in " VD_KernalFile "\n",
                 (unsigned long)crc);
   return false;
}

// Handler start (the C64 is already running): only re-stage the KERNAL bus hook. No storage access here.
void VD_AltButtonInit();          // alt-button disk switching, below
void VD_AltPoll();

FLASHMEM void InitHndlr_VDrive()
{
   if (vdProbing)                   // the menu did not finish the probe (not with this menu): numbers back, the KERNAL's own scan
   {                                // settles them; InitHndlr_KernalReplace() below hooks the replacement KERNAL in
      vdProbing = false;
      VD_LoadDevices();
      vio_arm_scan(vio);
      vio_publish_devices(vio);
   }
   InitHndlr_KernalReplace();
   VD_AltButtonInit();
   vdDrivePoll = &PollingHndlr_VDrive;
}
#else
void VD_AltPoll();
#endif // Fab04_VDrive

// ---- drive LED: the TR's LED blinks while the drives are busy (by ziggystar12, from his Mean Hamster Engine VDrive build) ----
// Each serviced mailbox command for our units restarts a short hold; while it runs the LED blinks against the state it had
// before (the TR keeps it on while a program runs, a pause or some cartridges turn it off), and gets that state back after.
#define VD_LedPin      34    // SetLEDOn / SetLEDOff (Common_Defs.h)
#define VD_LedBlinkMs  75
#define VD_LedHoldMs   180

static bool     vdLedActive = false, vdLedIdleHigh = false;
static uint32_t vdLedStartedMs = 0, vdLedLastAccessMs = 0;

FLASHMEM static void VD_LedPoll()
{
   if (!vdLedActive) return;
   uint32_t now = millis();
   if (now - vdLedLastAccessMs >= VD_LedHoldMs)
   {
      digitalWriteFast(VD_LedPin, vdLedIdleHigh);
      vdLedActive = false;
      return;
   }
   bool oddPhase = ((now - vdLedStartedMs) / VD_LedBlinkMs) & 1u;
   digitalWriteFast(VD_LedPin, oddPhase ? vdLedIdleHigh : !vdLedIdleHigh);
}

FLASHMEM static void VD_LedAccess()
{
   uint32_t now = millis();
   if (!vdLedActive)
   {
      vdLedIdleHigh = digitalReadFast(VD_LedPin) != 0;
      vdLedStartedMs = now;
      vdLedActive = true;
   }
   vdLedLastAccessMs = now;
   VD_LedPoll();
}

// what every VDrive handler polls; did = a mailbox command was answered just now (vio_poll, or the agent's vvec_poll)
FLASHMEM static void VD_PollCommon(bool did)
{
   VD_AltPoll();
   VD_LedPoll();
   if (did)
   {
      lastActivityMs = millis();
      if (vioHot.stat != VIO_NOTMINE) VD_LedAccess();     // not for a command handed back to a real drive on the bus
#ifdef VD_TRACE   // bring-up aid: one line per mailbox command on the USB serial port (see docs/BRINGUP.md)
      Serial.printf("vd cmd=%u arg=%02x len=%u -> stat=%02x data=%02x st=%02x\n",
                    vioHot.cmd, vioHot.arg, vioHot.len, vioHot.stat, vioHot.data, vioHot.st);
#endif
   }
   if (vio_take_menu(vio))          // VIO_MENU (VDrive Config's Q): write back what is unsaved, then the main loop resets into the menu
   {
      for (int i = 0; i < 2; i++) if (vdUnit[i].dirty) VD_Flush(&vdUnit[i]);
      BtnPressed = true;
   }
   // Write back only when both the last sector write and the last mailbox command are old: SD access can mask
   // interrupts, and the C64 must not be mid-LOAD (reading our registers) when that happens.
   uint32_t now = millis();
   for (int i = 0; i < 2; i++)
      if (vdUnit[i].dirty && now - vdUnit[i].lastWriteMs > VD_FlushIdleMs && now - lastActivityMs > VD_FlushIdleMs)
         VD_Flush(&vdUnit[i]);
}

#ifdef Fab04_VDrive
FLASHMEM void PollingHndlr_VDrive()
{
   if (vio == NULL) return;
   VD_PollCommon(vio_poll(vio) != 0);
}
#endif

// ---- remote control: the get / set commands on the TCP 2112 / USB serial port (SerUSBIO.ino) ------------------------------
// Both run in the main loop, between polls, so they never see a half-processed mailbox command. A change of disk waits for a
// quiet moment first (the C64 may be in the middle of a LOAD or SAVE); VD_Quiet() says when.
#define VD_QuietMs      40     // no mailbox command for this long, and no LOAD / SAVE / bus transaction open
#define VD_StaleMs      1000   // ... or nothing at all for this long: whatever was open has been abandoned
#define VD_SwapWaitMs   3000   // give up waiting for a quiet moment after this long

FLASHMEM static bool VD_Quiet()
{
   uint32_t idle = millis() - lastActivityMs;
   if (idle >= VD_StaleMs) return true;
   return idle >= VD_QuietMs && !vioHot.pending && vdrive_active(&vio->vd) < 0 && !vio->fl_active && !vio->fs_active;
}

static bool VD_RemoteReady(int unit)
{
   return vio != NULL && vioReady && unit >= 0 && unit <= 1 && vio->vd.unit[unit].present;
}

// One-line JSON for a unit; returns its length (0 = no such unit or VDrive not running).
FLASHMEM size_t VDrive_RemoteStatus(int unit, char *out, size_t cap)
{
   if (!VD_RemoteReady(unit)) return 0;
   stcVDUnit *u = &vdUnit[unit];
   char path[VD_PathLen];
   path[0] = 0;
   if (u->mounted) VD_HostPath(u, u->imgName, path, sizeof path);
   for (char *p = path; *p; p++) if (*p == '"' || *p == '\\') *p = '?';      // FAT cannot hold either; keep the JSON valid regardless
   char cwd[VD_CwdLen];
   strlcpy(cwd, u->cwd, sizeof cwd);
   for (char *p = cwd; *p; p++) if (*p == '"' || *p == '\\') *p = '?';
   int n = snprintf(out, cap,
      "{\"unit\":%d,\"storage\":\"%s\",\"device\":%u,\"fixed\":%u,\"mounted\":%s,\"writable\":%s,\"path\":\"%s\",\"cwd\":\"%s\"}",
      unit, unit == 0 ? "sd" : "usb", vio->vd.dev[unit], vio->vd.pin[unit], u->mounted ? "true" : "false",
      (u->mounted && u->d.write) ? "true" : "false", path, cwd[0] ? cwd : "/");
   return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

// Wait (up to VD_SwapWaitMs) for a quiet moment, polling the mailbox meanwhile, so the caller may touch the card: 0 = quiet now,
// 2 = the C64 kept the drive busy (reason in msg). Used by the swap below and by the remote directory listing.
FLASHMEM int VDrive_RemoteQuiet(char *msg, size_t cap)
{
   if (vio == NULL || !vioReady) { snprintf(msg, cap, "VDrive is not running"); return 1; }
   uint32_t began = millis();
   while (!VD_Quiet())
   {
      if (millis() - began > VD_SwapWaitMs) { snprintf(msg, cap, "C64 is busy with the drive"); return 2; }
      if (vdDrivePoll) vdDrivePoll();  // keep answering the C64 while we wait
   }
   msg[0] = 0;
   return 0;
}

// 0 = a readable file of at least D64 size; else 62 (not found) / 74 (not a disk image), reason in msg.
FLASHMEM static int VD_CheckImage(FS *fs, const char *path, char *msg, size_t cap)
{
   File probe = fs->open(path, FILE_READ);
   if (!probe) { snprintf(msg, cap, "file not found"); return 62; }
   bool bad = probe.isDirectory() || probe.size() < 174848;
   probe.close();
   if (bad) { snprintf(msg, cap, "not a disk image"); return 74; }
   return 0;
}

// Select the disk image for a unit: path = "/dir/IMAGE.D64" (ASCII, on that unit's own card), "" = unmount (the folder stays).
// Waits for a quiet moment (up to VD_SwapWaitMs, polling the mailbox meanwhile), then closes every channel on the unit and
// mounts, exactly as CD: would. Returns 0, or a DOS error code with a short reason in msg; 1 = no such unit, 2 = not quiet in time.
FLASHMEM int VDrive_RemoteSet(int unit, const char *path, char *msg, size_t cap)
{
   if (!VD_RemoteReady(unit)) { snprintf(msg, cap, "no such drive"); return 1; }
   stcVDUnit *u = &vdUnit[unit];
   if (path[0] && path[0] != '/') { snprintf(msg, cap, "path must start with /"); return 33; }
   if (path[0])   // check the image before disturbing anything: a mistyped path must leave the current disk mounted
   {
      int r = VD_CheckImage(u->fs, path, msg, cap);
      if (r) return r;
   }
   if (VDrive_RemoteQuiet(msg, cap)) return 2;
   if (!path[0])
   {
      VD_Unmount(NULL, unit);
      vdrive_attach(&vio->vd, unit, NULL, 1);
      vio_publish_devices(vio);
      msg[0] = 0;
      return 0;
   }
   char oldCwd[VD_CwdLen], oldImg[VD_CwdLen];
   strlcpy(oldCwd, u->cwd, sizeof oldCwd);
   strlcpy(oldImg, u->mounted ? u->imgName : "", sizeof oldImg);
   int r = VD_MountPath(unit, path);
   if (r)
   {
      strlcpy(u->cwd, oldCwd, sizeof u->cwd);                       // put the previous disk back
      d64_t *d = NULL;
      char old[VD_PathLen];
      VD_HostPath(u, oldImg, old, sizeof old);
      if (oldImg[0] && VD_MountHost(unit, old, &d) == 0) vdrive_attach(&vio->vd, unit, d, 1);
      else vdrive_attach(&vio->vd, unit, NULL, 1);
      snprintf(msg, cap, "cannot mount (DOS error %d)", r);
      return r;
   }
   msg[0] = 0;
   return 0;
}

// ---- remote control while VDrive is NOT running (the TR menu is showing) ------------------------------------------------------
// Nothing is mounted then; what a unit will mount at the next start is its /.vdrive sidecar (unless the launch comes from a disk image
// opened in the menu). So "get" reports that remembered image and "set" rewrites the sidecar: no handler, no mailbox, only the card.
FLASHMEM static FS *VD_IdleFS(int unit)
{
   if (unit == 0) return &SD;
   if (unit == 1 && USBFileSystemWait()) return &firstPartition;     // waits up to 1.5 s, as the other remote commands do
   return NULL;
}

FLASHMEM size_t VDrive_RemoteIdleStatus(int unit, char *out, size_t cap)
{
   if (unit < 0 || unit > 1) return 0;
   FS *fs = VD_IdleFS(unit);
   char path[VD_PathLen];
   path[0] = 0;
   if (fs)
   {
      File f = fs->open(VD_Sidecar, FILE_READ);
      if (f) { int n = f.read(path, sizeof path - 1); f.close(); path[n > 0 ? n : 0] = 0; }
      VD_Squeeze(path);
      if (path[0])
      {
         File probe = fs->open(path, FILE_READ);
         bool ok = probe && !probe.isDirectory();
         if (probe) probe.close();
         if (!ok) path[0] = 0;                                        // remembered image is gone: it will start unmounted
      }
   }
   for (char *p = path; *p; p++) if (*p == '"' || *p == '\\') *p = '?';
   char cwd[VD_PathLen];
   const char *slash = strrchr(path, '/');
   size_t cl = slash ? (size_t)(slash - path) : 0;
   memcpy(cwd, path, cl); cwd[cl] = 0;
   uint8_t pin = EEPROM.read(eepAdVDriveDevs + unit);                 // 0 = automatic
   if (pin < 8 || pin > 30) pin = 0;
   int n = snprintf(out, cap,
      "{\"unit\":%d,\"storage\":\"%s\",\"running\":false,\"device\":%u,\"fixed\":%u,\"mounted\":%s,\"writable\":true,\"path\":\"%s\",\"cwd\":\"%s\"}",
      unit, unit == 0 ? "sd" : "usb", pin, pin, path[0] ? "true" : "false", path, cl ? cwd : "/");
   return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

// Same arguments and return codes as VDrive_RemoteSet (1 = no such unit / card, 33, 62, 74), but only the sidecar changes.
FLASHMEM int VDrive_RemoteIdleSet(int unit, const char *path, char *msg, size_t cap)
{
   FS *fs = (unit >= 0 && unit <= 1) ? VD_IdleFS(unit) : NULL;
   if (!fs) { snprintf(msg, cap, "no such drive"); return 1; }
   char sq[VD_PathLen];
   if (strlen(path) >= sizeof sq) { snprintf(msg, cap, "path too long"); return 33; }
   strlcpy(sq, path, sizeof sq);
   VD_Squeeze(sq);
   if (sq[0] && sq[0] != '/') { snprintf(msg, cap, "path must start with /"); return 33; }
   if (sq[0])
   {
      int r = VD_CheckImage(fs, sq, msg, cap);
      if (r) return r;
   }
   if (fs->exists(VD_Sidecar)) fs->remove(VD_Sidecar);                // FILE_WRITE appends: start from an empty file
   if (sq[0])
   {
      File f = fs->open(VD_Sidecar, FILE_WRITE);
      if (!f) { snprintf(msg, cap, "cannot write /.vdrive"); return 74; }
      f.write(sq, strlen(sq));
      f.close();
   }
   msg[0] = 0;
   return 0;
}

// ---- alt button: cycle through the disk images listed in /vdrive-config.txt ----------------------------------------------------
// One tap picks the next image of the SD card's list, two taps within VD_TapWindowMs the USB drive's list; each list is the file
// /vdrive-config.txt in the root of its own drive. One image per line:   /path/IMAGE.D64, colour, flashes
// (colour = C64 colour 0..15 for the border, default 1; flashes = how many times, default 1; '#' starts a comment line). After the
// change the border flashes that way (250 ms on, 750 ms off) so the switch can be seen without a screen full of text; a card or
// stick that is missing, an unreadable list or no usable image in it flashes red quickly for 2 s. An image that has disappeared is
// skipped. With nothing mounted (or an image that is not in the list) the first usable image is picked.
// Wired to the TR's Alt button only when its setting (TR menu, settings) is "None": a chosen action is never taken over.
// The border change itself is done by the KERNAL's IRQ routine (patches/vdrive.s, irq_border), driven by the BORDER register.
#define VD_CfgBufSize    2048
#define VD_CfgMaxEntries 32
#define VD_TapWindowMs   1000
#define VD_FlashOnMs     250
#define VD_FlashOffMs    750
#define VD_ErrFlashMs    100    // red error flash: 100 ms on / 100 ms off for 2 s
#define VD_ErrFlashes    10
#define VD_SettleMs      100    // the IRQ routine must see a SAVE / RESTORE request at least once (every 16-20 ms)
#define VD_ColorRed      2

struct stcVDCfgEntry { char *path; uint8_t color, flashes; };

static char          vdCfgBuf[VD_CfgBufSize];
static stcVDCfgEntry vdCfg[VD_CfgMaxEntries];

// "...,  12 " -> 12 and the line cut before the comma; false (line untouched) when the last field is not a plain number
FLASHMEM static bool VD_TrailingNumber(char *line, int *val)
{
   char *comma = strrchr(line, ',');
   if (!comma) return false;
   const char *p = comma + 1;
   while (*p == ' ' || *p == '\t') p++;
   if (!*p) return false;
   int v = 0, digits = 0;
   for (; *p >= '0' && *p <= '9'; p++) { v = v * 10 + (*p - '0'); if (++digits > 3) return false; }
   while (*p == ' ' || *p == '\t') p++;
   if (digits == 0 || *p) return false;
   *comma = 0;
   *val = v;
   return true;
}

// Parses the text in place; returns the number of usable entries (lines that are not a "/path" are skipped).
FLASHMEM static int VD_ParseConfig(char *text, stcVDCfgEntry *out, int max)
{
   int n = 0;
   while (*text && n < max)
   {
      char *line = text;
      while (*text && *text != '\n') text++;
      if (*text) *text++ = 0;
      char *end = line + strlen(line);
      while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
      while (*line == ' ' || *line == '\t') line++;
      if (*line != '/') continue;                                 // comment, blank or not a path
      int a = -1, b = -1;
      if (VD_TrailingNumber(line, &a) && VD_TrailingNumber(line, &b)) { int t = a; a = b; b = t; }   // "path, colour, flashes"
      else if (a >= 0) { b = -1; }                                // "path, colour"
      end = line + strlen(line);
      while (end > line && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
      if (end == line) continue;
      out[n].path = line;
      out[n].color = (a < 0) ? 1 : (uint8_t)(a & 0x0F);
      out[n].flashes = (b < 0) ? 1 : (uint8_t)(b > 20 ? 20 : b);
      n++;
   }
   return n;
}

// ---- border flash sequencer (main loop, driven from PollingHndlr_VDrive) ----
enum { VDF_Idle, VDF_Save, VDF_On, VDF_Off, VDF_Restore };
struct stcVDFlash
{
   uint8_t  phase, color, left;
   uint16_t onMs, offMs;
   uint32_t since;
   bool     queued;            // a new sequence waits for the running one to give the colour back
   uint8_t  qColor, qCount;
   uint16_t qOnMs, qOffMs;
};
static stcVDFlash vdFlash;

FLASHMEM static void VD_FlashPhase(uint8_t phase, uint8_t reg)
{
   vdFlash.phase = phase;
   vdFlash.since = millis();
   vioHot.border = reg;
}

FLASHMEM static void VD_FlashBegin(uint8_t color, uint8_t count, uint16_t onMs, uint16_t offMs)
{
   vdFlash.color = color; vdFlash.left = count; vdFlash.onMs = onMs; vdFlash.offMs = offMs;
   if (count == 0) { VD_FlashPhase(VDF_Idle, VIO_BRD_IDLE); return; }
   VD_FlashPhase(VDF_Save, VIO_BRD_SAVE);
}

FLASHMEM static void VD_Flash(uint8_t color, uint8_t count, uint16_t onMs, uint16_t offMs)
{
   if (vdFlash.phase == VDF_Idle) { VD_FlashBegin(color, count, onMs, offMs); return; }
   vdFlash.queued = true;                                         // saving now would remember the flash colour: restore first
   vdFlash.qColor = color; vdFlash.qCount = count; vdFlash.qOnMs = onMs; vdFlash.qOffMs = offMs;
   VD_FlashPhase(VDF_Restore, VIO_BRD_RESTORE);
}

FLASHMEM static void VD_FlashPoll()
{
   if (vdFlash.phase == VDF_Idle) return;
   uint32_t held = millis() - vdFlash.since;
   switch (vdFlash.phase)
   {
      case VDF_Save:
         if (held >= VD_SettleMs) VD_FlashPhase(VDF_On, VIO_BRD_SET | vdFlash.color);
         break;
      case VDF_On:
         if (held >= vdFlash.onMs) { vdFlash.left--; VD_FlashPhase(VDF_Off, VIO_BRD_RESTORE); }
         break;
      case VDF_Off:
         if (vdFlash.left == 0 ? held >= VD_SettleMs : held >= vdFlash.offMs)
         {
            if (vdFlash.left == 0) VD_FlashPhase(VDF_Idle, VIO_BRD_IDLE);     // the last flash needs no pause behind it
            else VD_FlashPhase(VDF_On, VIO_BRD_SET | vdFlash.color);
         }
         break;
      case VDF_Restore:                                            // aborted sequence: colour given back, then the queued one
         if (held >= VD_SettleMs)
         {
            VD_FlashPhase(VDF_Idle, VIO_BRD_IDLE);
            if (vdFlash.queued) { vdFlash.queued = false; VD_FlashBegin(vdFlash.qColor, vdFlash.qCount, vdFlash.qOnMs, vdFlash.qOffMs); }
         }
         break;
   }
}

FLASHMEM static void VD_FlashError() { VD_Flash(VD_ColorRed, VD_ErrFlashes, VD_ErrFlashMs, VD_ErrFlashMs); }

// ---- the switch itself ----
FLASHMEM static bool VD_SameImage(const char *a, const char *b)
{
   char x[VD_PathLen], y[VD_PathLen];
   strlcpy(x, a, sizeof x); strlcpy(y, b, sizeof y);
   VD_Squeeze(x); VD_Squeeze(y);
   return strcasecmp(x, y) == 0;
}

// Select the next usable image of the unit's list; flashes the result. Returns true when an image was mounted.
FLASHMEM static bool VD_SwitchNext(int unit)
{
   char msg[64];
   if (!VD_RemoteReady(unit)) { VD_FlashError(); return false; }           // no card / stick (or VDrive not running)
   stcVDUnit *u = &vdUnit[unit];
   if (VDrive_RemoteQuiet(msg, sizeof msg)) { VD_FlashError(); return false; }   // card access only at a quiet moment
   File f = u->fs->open(VD_ConfigName, FILE_READ);
   if (!f || f.isDirectory()) { if (f) f.close(); VD_FlashError(); return false; }
   int len = f.read(vdCfgBuf, VD_CfgBufSize - 1);
   f.close();
   if (len <= 0) { VD_FlashError(); return false; }
   vdCfgBuf[len] = 0;
   int n = VD_ParseConfig(vdCfgBuf, vdCfg, VD_CfgMaxEntries);
   if (n == 0) { VD_FlashError(); return false; }

   int start = 0;                                                          // nothing mounted, or not in the list: the first one
   if (u->mounted)
   {
      char cur[VD_PathLen];
      VD_HostPath(u, u->imgName, cur, sizeof cur);
      for (int i = 0; i < n; i++) if (VD_SameImage(cur, vdCfg[i].path)) { start = (i + 1) % n; break; }
   }
   for (int k = 0; k < n; k++)
   {
      stcVDCfgEntry *e = &vdCfg[(start + k) % n];
      if (VD_CheckImage(u->fs, e->path, msg, sizeof msg)) continue;       // gone (or not an image): next one
      uint8_t color = e->color, count = e->flashes;
      if (VDrive_RemoteSet(unit, e->path, msg, sizeof msg) == 0) { VD_Flash(color, count, VD_FlashOnMs, VD_FlashOffMs); return true; }
      if (strstr(msg, "busy")) break;                                      // the C64 never let go of the drive
   }
   VD_FlashError();
   return false;
}

// ---- Alt button: tap counting ----
static uint8_t  vdTaps = 0;
static uint32_t vdTapFirstMs = 0;
static bool     vdSwitching = false;

FLASHMEM void SpecialBtn_VDrive(bool Up_nDn)
{
   if (Up_nDn) return;                                // act on the press
   if (vdTaps == 0) vdTapFirstMs = millis();
   if (vdTaps < 2) vdTaps++;
}

FLASHMEM void VD_AltPoll()
{
   VD_FlashPoll();
   if (vdTaps == 0 || vdSwitching) return;
   if (vdTaps < 2 && millis() - vdTapFirstMs < VD_TapWindowMs) return;     // a second tap may still come
   int unit = vdTaps >= 2 ? 1 : 0;
   vdTaps = 0;
   vdSwitching = true;                                // VDrive_RemoteQuiet() polls: do not re-enter from there
   VD_SwitchNext(unit);
   vdSwitching = false;
}

extern void (*fSpecialBtnChange)(bool Up_nDn);
FLASHMEM void VD_AltButtonInit()
{
   vdTaps = 0; vdFlash.phase = VDF_Idle; vdFlash.queued = false;
#ifdef Fab04_SpecialButton
   if ((IO1[rwRegPwrUpDefaults2] & rpud2AltBtnActionMask) == rpud2AltBtnNone) fSpecialBtnChange = &SpecialBtn_VDrive;
#endif
}

static_assert(offsetof(vio_hot_t, cmd) == VIO_CMD && offsetof(vio_hot_t, arg) == VIO_ARG && offsetof(vio_hot_t, data) == VIO_DATA &&
              offsetof(vio_hot_t, stat) == VIO_STAT && offsetof(vio_hot_t, st) == VIO_ST && offsetof(vio_hot_t, len) == VIO_LEN &&
              offsetof(vio_hot_t, dev0) == VIO_DEV0 && offsetof(vio_hot_t, dev1) == VIO_DEV1 && offsetof(vio_hot_t, border) == VIO_BORDER &&
              offsetof(vio_hot_t, id0) == VIO_ID0 && offsetof(vio_hot_t, id1) == VIO_ID1, "register bytes must stay in register order");

#ifdef Fab04_VDrive
FASTRUN void IO1Hndlr_VDrive(uint8_t Address, bool R_Wn)
{
   Address -= VD_MboxBase;                                 // wraps for offsets below the base
   if (!vioReady || Address >= VIO_NREGS) return;         // not mapped: open bus
   // Read straight from the register bytes (cmd..dev1 are laid out in register order): every cycle before the data lines are
   // driven eats into the 6510's setup time, and the call + switch of vio_io1_read() made this path the slowest one (about one
   // wrong byte in 8000 reads on a real C64, see tests/asm/bustest.s).
   if (R_Wn) DataPortWriteWaitLog(reinterpret_cast<volatile uint8_t *>(&vioHot)[Address]);
   else
   {
      uint8_t Data = DataPortWaitRead();
      TraceLogAddValidData(Data);
      vio_io1_write(&vioHot, Address, Data);
   }
}
#endif

FASTRUN void IO2Hndlr_VDrive(uint8_t Address, bool R_Wn)
{
   Address -= VIO_WIN_OFS;                                 // window at $DF80-$DFFF; $DF00-$DF7F wraps past it: open bus
   if (!vioReady || Address >= VIO_WIN) return;           // (the REU's registers in IOH_VDriveREU.c)
   if (R_Wn) DataPortWriteWaitLog(vioHot.window[Address]);   // straight from the struct, as in IO1Hndlr_VDrive
   else
   {
      uint8_t Data = DataPortWaitRead();
      TraceLogAddValidData(Data);
      vio_io2_write(&vioHot, Address, Data);
   }
}
