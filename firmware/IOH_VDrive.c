// "VDrive" Special IO for TeensyROM+: replacement KERNAL + virtual IEC drive.
//   unit 0 = SD card, unit 1 = USB drive. Device numbers default to 8/9 and are kept in EEPROM; change them from
//   BASIC with  OPEN15,8,15,"U0>"+CHR$(10):CLOSE15  (this drive becomes device 10; CHR$(0) restores the default); each serves a mounted .d64 (or lists the host
//   directory so one can be picked with  OPEN15,8,15,"CD:GAME.D64"  /  "CD:_" to unmount).
// Includes the KERNAL bus hook from IOH_KernalReplace.c (select the patched 8K image with
// <Shift-K> as usual) and adds the IO1/IO2 mailbox the patched KERNAL talks to.
//
// This file is included from IOHandlers.h like the other handlers (unity build, compiled as C++).

// Bring-up phase: log every mailbox command on the USB serial port. Define VD_NO_TRACE (e.g. at the top of this
// file) to silence it once things work: the printing slows the polling loop.
#if !defined(VD_NO_TRACE) && !defined(VD_TRACE)
#define VD_TRACE
#endif

#define VD_COLD FLASHMEM          // everything except the bus-ISR register accessors lives in flash
#include "../vdrive/d64.c"
#include "../vdrive/vdrive.c"
#include "../vdrive/vdrive_io.c"

void VDrive_PreStart();   // called from KERNALPreStart(), see StatusFunctions.c
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

FLASHMEM static void VD_PathFor(stcVDUnit *u, const char *name, char *out, size_t cap)
{
   size_t n = snprintf(out, cap, "%s/", u->cwd);
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
   if (u->fs->exists(VD_Sidecar)) u->fs->remove(VD_Sidecar);
}

FLASHMEM static void VD_SidecarSave(stcVDUnit *u, const char *path)
{
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
   d64_open(&u->d, VD_ReadSector, u, size >= 196608 ? 40 : 35);
   u->d.write = writable ? VD_WriteSector : NULL;
   u->mounted = true; u->dirty = false;
   *out = &u->d;
   VD_SidecarSave(u, path);
   Printf_dbg("VDrive %d: mounted %s%s\n", unit + 8, path, writable ? "" : " (read-only)");
   return 0;
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

// ---- device numbers in EEPROM (0 / out of range = default VD_DEV0/VD_DEV1) --------------------------
FLASHMEM static void VD_SaveDevices(void *ctx, uint8_t dev0, uint8_t dev1)
{
   EEPROM.write(eepAdVDriveDevs, dev0);        // written by "U0>"+CHR$(n) on the drive's command channel
   EEPROM.write(eepAdVDriveDevs + 1, dev1);
}

FLASHMEM static void VD_LoadDevices()
{
   uint8_t d0 = EEPROM.read(eepAdVDriveDevs), d1 = EEPROM.read(eepAdVDriveDevs + 1);
   if (d0 < 8 || d0 > 30) d0 = VD_DEV0;        // 0 (unset) or garbage -> default
   if (d1 < 8 || d1 > 30) d1 = VD_DEV1;
   if (d0 == d1) { d0 = VD_DEV0; d1 = VD_DEV1; }
   vdrive_set_devices(&vio->vd, d0, d1);
}

// ---- mount at start-up: the image opened in the TeensyROM menu, else the one remembered in the sidecar ------------------
// Opening a .d64 in the menu makes DriveDirPath "<dir>/<image>.d64*" (trailing '*' = inside an image). When a program is
// launched from there, that image is mounted on the unit of the same storage (SD = unit 0, USB = unit 1) so the program's own
// LOAD from device 8/9 finds its files. Any other launch re-mounts what the unit had mounted last.
extern char DriveDirPath[];
extern volatile uint8_t* IO1;     // defined in IOH_TeensyROM.c, which is included after this file

// path = "/a/b/X.D64" (ASCII, no trailing '*'): folder becomes the unit's cwd, then the image is mounted.
FLASHMEM static void VD_MountPath(int unit, const char *path)
{
   stcVDUnit *u = &vdUnit[unit];
   const char *slash = strrchr(path, '/');
   const char *file = slash ? slash + 1 : path;
   size_t dl = slash ? (size_t)(slash - path) : 0;                 // "/a/b/X.D64" -> cwd "/a/b"; root -> ""
   if (dl >= VD_CwdLen || !*file) return;
   memcpy(u->cwd, path, dl); u->cwd[dl] = 0;
   char pname[VD_CwdLen];                                           // VD_Mount takes PETSCII names
   size_t i = 0;
   for (; file[i] && i + 1 < sizeof pname; i++) pname[i] = (char)VD_ToPetscii(file[i]);
   pname[i] = 0;
   d64_t *d = NULL;
   if (VD_Mount(NULL, unit, pname, &d) == 0) vdrive_attach(&vio->vd, unit, d, 1);
   else u->cwd[0] = 0;
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
   vdrive_set_host(&vio->vd, &host);
   VD_LoadDevices();

   for (int i = 0; i < 2; i++) { vdUnit[i].mounted = false; vdUnit[i].dirty = false; vdUnit[i].cwd[0] = 0; }
   vdUnit[0].fs = &SD;
   vdUnit[1].fs = &firstPartition;
   vdrive_attach(&vio->vd, 0, NULL, SDFullInit());
   vdrive_attach(&vio->vd, 1, NULL, USBFileSystemWait());
   VD_StartupMount(0, IO1[rWRegCurrMenuWAIT] == rmtSD);
   VD_StartupMount(1, IO1[rWRegCurrMenuWAIT] == rmtUSBDrive);
   vioReady = true;                 // only now may the bus handlers answer
   Serial.printf("VDrive: dev %d (SD) %s, dev %d (USB) %s\n",
                 vio->vd.dev[0], vio->vd.unit[0].present ? "ready" : "absent",
                 vio->vd.dev[1], vio->vd.unit[1].present ? "ready" : "absent");
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

FASTRUN void IO1Hndlr_VDrive(uint8_t Address, bool R_Wn)
{
   if (!vioReady || Address >= VIO_NREGS) return;         // not mapped: open bus
   if (R_Wn) DataPortWriteWaitLog(vio_io1_read(&vioHot, Address));
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
