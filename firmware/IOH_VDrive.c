// "VDrive" Special IO for TeensyROM+: replacement KERNAL + virtual IEC drive.
//   unit 0 = SD card, unit 1 = USB drive. Device numbers are automatic by default: the first free numbers from 8 up
//   (the KERNAL probes the real serial bus on the first disk access of a run, see vdrive_io.h), so 8 (SD) and 9 (USB) when
//   no other drive is attached. A number can be fixed from BASIC with  OPEN15,8,15,"U0>"+CHR$(10):CLOSE15  (this drive becomes
//   device 10, kept in EEPROM; CHR$(0) makes it automatic again). Each unit serves a mounted .d64 / .d71 / .d81 (or lists the host
//   directory so one can be picked with  OPEN15,8,15,"CD:GAME.D64"  /  "CD:_" to unmount, or  LOAD"$/GAMES/GAME.D64",8).
// Includes the KERNAL bus hook from IOH_KernalReplace.c (select the patched 8K image with
// <Shift-K> as usual) and adds the IO1/IO2 mailbox the patched KERNAL talks to.
//
// This file is included from IOHandlers.h like the other handlers (unity build, compiled as C++).

// Optional bring-up aid: define VD_TRACE (e.g. at the top of this file) to log every mailbox command on the USB serial
// port. Off by default: the print runs right after the answer is published, i.e. while the C64 is still reading ST / DATA,
// and the USB serial interrupts then make the bus handler answer late (the C64 reads $00 / the previous bus byte).

#define VD_COLD FLASHMEM          // everything except the bus-ISR register accessors lives in flash
#include "../vdrive/d64.c"
#include "../vdrive/vdrive.c"
#include "../vdrive/vdrive_io.c"

void VDrive_PreStart();   // called from KERNALPreStart(), see StatusFunctions.c
uint8_t VDriveSwift_Remap(uint8_t ioh);   // IOH_VDriveSwift.c; used by KERNALPreStart() and IOHandlerSelectInit()
void InitHndlr_VDrive();
void PollingHndlr_VDrive();
void IO1Hndlr_VDrive(uint8_t Address, bool R_Wn);
void IO2Hndlr_VDrive(uint8_t Address, bool R_Wn);

stcIOHandlers IOHndlr_VDrive =
{
  "VDrive KERNAL",          //Name of handler (IOHNameLength max)
  &InitHndlr_VDrive,        //Called once at handler startup
  &IO1Hndlr_VDrive,         //IO1 R/W handler: mailbox registers
  &IO2Hndlr_VDrive,         //IO2 R/W handler: 256-byte data window
  NULL,                     //ROML Read handler, in addition to any ROM data sent
  NULL,                     //ROMH Read handler, in addition to any ROM data sent
  &PollingHndlr_VDrive,     //Polled in main routine: does all file system work
  NULL,                     //called at the end of EVERY c64 cycle
};

#define VD_MboxBase    0x10  // mailbox at $DE10-$DE15: $DE00-$DE07 stays free for the SwiftLink ACIA (see IOH_VDriveSwift.c)
#define VD_CwdLen      96
#define VD_FlushIdleMs 250   // write the image back this long after the last sector write

struct stcVDUnit
{
   FS      *fs;              // SD or USB file system
   File     img;             // mounted .d64
   File     dir;             // host directory being listed
   d64_t    d;
   bool     mounted;
   bool     dirty;
   bool     quiet;           // temporary selection for a listing by path: leave the /.vdrive sidecar alone
   char     imgName[VD_CwdLen];   // PETSCII name the mounted image was opened with (in cwd)
   char     snapCwd[VD_CwdLen];   // folder / image selected before a listing by path (push / pop)
   char     snapImg[VD_CwdLen];   // "" = nothing was mounted
   uint32_t lastWriteMs;
   char     cwd[VD_CwdLen];  // "" = root
};

// The drive state (~4.6 KB, see VIO_RAM_BUDGET in tests/test_vio.py) lives in RAM2 by default so RAM1 is
// untouched; define VD_ALLOC_HEAP to take it from the malloc heap instead.
#ifndef VD_ALLOC_HEAP
DMAMEM static vio_t vioStore;
#endif
static vio_hot_t  vioHot;           // registers + window: the ONLY memory the bus ISR touches. Plain static = RAM1/DTCM, fast
static vio_t     *vio = NULL;       // everything else (main loop only, may live in slower RAM2)
static volatile bool vioReady = false;
static uint32_t   lastActivityMs = 0;
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
FLASHMEM static void VD_Unmount(void *ctx, int unit)
{
   stcVDUnit *u = &vdUnit[unit];
   VD_Flush(u);
   if (u->mounted) u->img.close();
   u->mounted = false;
   VD_SidecarClear(u);                                   // an explicit unmount forgets the image (a new mount re-writes it)
}

FLASHMEM static int VD_Mount(void *ctx, int unit, const char *name, d64_t **out)
{
   stcVDUnit *u = &vdUnit[unit];
   char path[VD_CwdLen + 20];
   VD_PathFor(u, name, path, sizeof path);
   VD_Unmount(ctx, unit);
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
   u->mounted = true; u->dirty = false;
   strlcpy(u->imgName, name, sizeof u->imgName);
   *out = &u->d;
   VD_SidecarSave(u, path);
   Printf_dbg("VDrive %d: mounted %s%s\n", unit + 8, path, writable ? "" : " (read-only)");
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
         char path[VD_CwdLen + 20];
         VD_PathFor(u, u->imgName, path, sizeof path);
         VD_SidecarSave(u, path);
      }
      else VD_SidecarClear(u);
      return 0;
   }
   VD_Unmount(ctx, unit);
   strlcpy(u->cwd, u->snapCwd, sizeof u->cwd);
   *out = NULL;
   int r = 0;
   if (u->snapImg[0]) r = VD_Mount(ctx, unit, u->snapImg, out);
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
   char path[VD_CwdLen + 20];
   VD_PathFor(u, name, path, sizeof path);
   size_t l = strlen(path); if (l > 1 && path[l - 1] == '/') path[l - 1] = 0;   // drop trailing '/'
   File d = u->fs->open(path);
   if (!d || !d.isDirectory()) { if (d) d.close(); return 62; }
   d.close();
   if (strlen(path) >= VD_CwdLen) return 74;
   strcpy(u->cwd, path);
   return 0;
}

FLASHMEM static int VD_DirOpen(void *ctx, int unit, char title[17])
{
   stcVDUnit *u = &vdUnit[unit];
   if (u->dir) u->dir.close();
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
      const char *n = e.name();
      if (IsDotfile(n) || strlen(n) > 16) { e.close(); continue; }   // names the 16-char C64 listing can't hold
      size_t i = 0;
      for (; n[i]; i++) name[i] = VD_ToPetscii(n[i]);
      name[i] = 0;
      *is_dir = e.isDirectory();
      *size = *is_dir ? 0 : e.size();
      e.close();
      return 1;
   }
   u->dir.close();
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
extern volatile uint8_t* IO1;     // defined in IOH_TeensyROM.c, which is included after this file

// path = "/a/b/X.D64" (ASCII, no trailing '*'): folder becomes the unit's cwd, then the image is mounted.
FLASHMEM static int VD_MountPath(int unit, const char *rawPath)   // 0 ok, else a DOS error code
{
   stcVDUnit *u = &vdUnit[unit];
   char sq[VD_CwdLen + 20];
   if (strlen(rawPath) >= sizeof sq) return 33;
   strlcpy(sq, rawPath, sizeof sq);
   VD_Squeeze(sq);
   const char *path = sq;
   const char *slash = strrchr(path, '/');
   const char *file = slash ? slash + 1 : path;
   size_t dl = slash ? (size_t)(slash - path) : 0;                 // "/a/b/X.D64" -> cwd "/a/b"; root -> ""
   if (dl >= VD_CwdLen || !*file) return 33;
   memcpy(u->cwd, path, dl); u->cwd[dl] = 0;
   char pname[VD_CwdLen];                                           // VD_Mount takes PETSCII names
   size_t i = 0;
   for (; file[i] && i + 1 < sizeof pname; i++) pname[i] = (char)VD_ToPetscii(file[i]);
   pname[i] = 0;
   d64_t *d = NULL;
   int r = VD_Mount(NULL, unit, pname, &d);
   if (r == 0) vdrive_attach(&vio->vd, unit, d, 1);
   else u->cwd[0] = 0;
   return r;
}

FLASHMEM static void VD_StartupMount(int unit, bool menuUnit)
{
   stcVDUnit *u = &vdUnit[unit];
   if (!vio->vd.unit[unit].present) return;
   char path[VD_CwdLen + 20];
   if (menuUnit)
   {
      size_t n = strlen(DriveDirPath);
      if (n >= 2 && DriveDirPath[n - 1] == '*' && n < sizeof path)
      {
         memcpy(path, DriveDirPath, n - 1); path[n - 1] = 0;
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
void InitHndlr_KernalReplace();   // re-stages the KERNAL bus hook (IOH_KernalReplace.c)

// All storage access (SD/USB probing, sizing) happens here, called from KERNALPreStart() while the C64 is still
// waiting for the TR ("WaitForTRDots"). Doing it later is unsafe: SD reads mask interrupts, and once the replacement
// KERNAL is live every C64 instruction fetch from $E000-$FFFF depends on the bus ISR running.
FLASHMEM void VDrive_PreStart()
{
#ifdef VD_ALLOC_HEAP
   if (vio == NULL) vio = (vio_t *)malloc(sizeof(vio_t));
#else
   vio = &vioStore;                 // DMAMEM is not zero-initialised: vio_init() clears it
#endif
   if (vio == NULL) { Serial.println("VDrive: out of memory"); return; }
   vioReady = false;
   vio_init(vio, &vioHot);

   vd_host_t host;
   memset(&host, 0, sizeof host);
   host.mount = VD_Mount;  host.unmount = VD_Unmount;  host.chdir = VD_ChDir;
   host.dir_open = VD_DirOpen;  host.dir_next = VD_DirNext;  host.save_devices = VD_SaveDevices;
   host.push = VD_Push;  host.pop = VD_Pop;
   vdrive_set_host(&vio->vd, &host);
   VD_LoadDevices();

   for (int i = 0; i < 2; i++) { vdUnit[i].mounted = false; vdUnit[i].dirty = false; vdUnit[i].quiet = false; vdUnit[i].cwd[0] = 0; }
   vdUnit[0].fs = &SD;
   vdUnit[1].fs = &firstPartition;
   vdrive_attach(&vio->vd, 0, NULL, SDFullInit());
   vdrive_attach(&vio->vd, 1, NULL, USBFileSystemWait());
   VD_StartupMount(0, IO1[rWRegCurrMenuWAIT] == rmtSD);
   VD_StartupMount(1, IO1[rWRegCurrMenuWAIT] == rmtUSBDrive);
   int scanPending = vio_arm_scan(vio);   // automatic numbers: the KERNAL probes the real bus on the run's first disk access
   vio_publish_devices(vio);
   vioReady = true;                 // only now may the bus handlers answer
   Serial.printf("VDrive: dev %d (SD) %s, dev %d (USB) %s%s\n",
                 vio->vd.dev[0], vio->vd.unit[0].present ? "ready" : "absent",
                 vio->vd.dev[1], vio->vd.unit[1].present ? "ready" : "absent",
                 scanPending ? " (numbers are settled by the first bus scan)" : "");
}

// Handler start (the C64 is already running): only re-stage the KERNAL bus hook. No storage access here.
FLASHMEM void InitHndlr_VDrive()
{
   InitHndlr_KernalReplace();
}

FLASHMEM void PollingHndlr_VDrive()
{
   if (vio == NULL) return;
   if (vio_poll(vio))
   {
      lastActivityMs = millis();
#ifdef VD_TRACE   // bring-up aid: one line per mailbox command on the USB serial port (see docs/BRINGUP.md)
      Serial.printf("vd cmd=%u arg=%02x len=%u -> stat=%02x data=%02x st=%02x\n",
                    vioHot.cmd, vioHot.arg, vioHot.len, vioHot.stat, vioHot.data, vioHot.st);
#endif
   }
   // Write back only when both the last sector write and the last mailbox command are old: SD access can mask
   // interrupts, and the C64 must not be mid-LOAD (reading our registers) when that happens.
   uint32_t now = millis();
   for (int i = 0; i < 2; i++)
      if (vdUnit[i].dirty && now - vdUnit[i].lastWriteMs > VD_FlushIdleMs && now - lastActivityMs > VD_FlushIdleMs)
         VD_Flush(&vdUnit[i]);
}

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
   char path[VD_CwdLen + 20];
   path[0] = 0;
   if (u->mounted) VD_PathFor(u, u->imgName, path, sizeof path);
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
      PollingHndlr_VDrive();           // keep answering the C64 while we wait
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
      if (oldImg[0] && VD_Mount(NULL, unit, oldImg, &d) == 0) vdrive_attach(&vio->vd, unit, d, 1);
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
   char path[VD_CwdLen + 20];
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
   char cwd[VD_CwdLen + 20];
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
   char sq[VD_CwdLen + 20];
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

static_assert(offsetof(vio_hot_t, cmd) == VIO_CMD && offsetof(vio_hot_t, arg) == VIO_ARG && offsetof(vio_hot_t, data) == VIO_DATA &&
              offsetof(vio_hot_t, stat) == VIO_STAT && offsetof(vio_hot_t, st) == VIO_ST && offsetof(vio_hot_t, len) == VIO_LEN &&
              offsetof(vio_hot_t, dev0) == VIO_DEV0 && offsetof(vio_hot_t, dev1) == VIO_DEV1, "register bytes must stay in register order");

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

FASTRUN void IO2Hndlr_VDrive(uint8_t Address, bool R_Wn)
{
   if (!vioReady) return;
   if (R_Wn) DataPortWriteWaitLog(vio_io2_read(&vioHot, Address));
   else
   {
      uint8_t Data = DataPortWaitRead();
      TraceLogAddValidData(Data);
      vio_io2_write(&vioHot, Address, Data);
   }
}
