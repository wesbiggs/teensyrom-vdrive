/* Build-time knobs shared by d64.c / vdrive.c / vdrive_io.c. */
#ifndef VD_CFG_H
#define VD_CFG_H
/* Placement for code that is NOT reachable from the bus interrupt. The firmware
 * glue defines this as FLASHMEM so only the O(1) register accessors stay in RAM1. */
#ifndef VD_COLD
#define VD_COLD
#endif

/* IEC device numbers of the two virtual drives (unit 0 = SD, unit 1 = USB). Any other device number
 * keeps using the real serial bus, so e.g. -DVD_DEV0=10 -DVD_DEV1=11 leaves physical drives 8 and 9
 * alone. The KERNAL image contains no device numbers: change them here and rebuild the firmware only. */
#ifndef VD_DEV0
#define VD_DEV0 8
#endif
#ifndef VD_DEV1
#define VD_DEV1 9
#endif

/* Simultaneously open data channels per unit. Real 1541s have 5; the pool is shared by
 * LOAD (channel 0), SAVE (1), and OPENs 2-14. Each slot costs ~290 bytes. */
#ifndef VD_NSLOT
#define VD_NSLOT 6
#endif
#endif
