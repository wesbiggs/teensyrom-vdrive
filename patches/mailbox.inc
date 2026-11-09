; The TR mailbox: register layout, answer tokens and command numbers (d64/vdrive_io.h). Shared by the KERNAL patch
; (patches/vdrive.s) and the plain TeensyROM's agent (vector/vdagent.s). MBX / WIN may be set with -D.
; Constants only: including it emits no bytes.
.ifndef MBX
MBX = $DE10
.endif
.ifndef VD_DMA_LOAD
VD_DMA_LOAD = 1             ; 0: the LOAD loop asks for window blocks only (FLOAD_BLK), as in the upstream TeensyROM build (docs/DESIGN.md 49)
.endif
.ifndef WIN
WIN = $DF80                 ; 128-byte IO2 window: file names and data blocks ($DF00-$DF7F: REU registers in "VDrive+Swift+REU")
.endif
WINSZ    = 128              ; VIO_WIN in d64/vdrive_io.h
MBX_CMD  = MBX+0
MBX_ARG  = MBX+1
MBX_DATA = MBX+2
MBX_STAT = MBX+3
MBX_ST   = MBX+4
MBX_LEN  = MBX+5
MBX_BORDER = MBX+8
MBX_ID   = MBX+9
MBX_KEY  = MBX+11             ; W: the plain TR's keyboard hook (vector/vdhooks.s): a key pressed with CTRL+C=
VD_ID0   = $D6              ; VIO_ID0_VAL / VIO_ID1_VAL in d64/vdrive_io.h
VD_ID1   = $64
STAT_OK      = $5A      ; answer tokens: exact values, so a misread can't pass for "done"
STAT_NOTMINE = $A5
STAT_SCAN    = $3C      ; "scan the serial bus first, then send this command again" (only ever answered to a LISTEN, TALK, fast LOAD / SAVE open)

ST       = $90

C_LISTEN = 1
C_TALK   = 2
C_SECOND = 3
C_TKSA   = 4
C_CIOUT  = 5
C_ACPTR  = 6
C_UNLSN  = 7
C_UNTLK  = 8
C_FLOAD_OPEN  = 9
C_FLOAD_BLK   = 10
C_FSAVE_OPEN  = 11
C_FSAVE_BLK   = 12
C_FSAVE_CLOSE = 13
C_SCAN   = 14
C_FREE   = 15
C_MENU   = 16
C_FLOAD_DMA = 17
