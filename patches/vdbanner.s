; Power-on banner with the virtual drives' device numbers. The top line stays stock; the second becomes
;      64K RAM  38911 FREE  VDRIVE SD 8 USB 9
; one leading space and 38 characters, like the stock " 64K RAM SYSTEM  38911 BASIC BYTES FREE". The drive field is always 10 wide:
; "SD 8" / "SD10" / "NOSD", a space, "USB 9" / "USB11" / "NOUSB". Without the VDrive firmware the line is the stock one.
;
; BASIC's start-up message routine (INITMS, $E422, in the KERNAL chip) prints the banner string at $E473 (`JSR STROUT` at $E42D),
; the free bytes ($BDCD, no leading space), then " BASIC BYTES FREE" + CR ($E460, from $E43D) and ends in `JMP $A644` (NEW). The banner
; string now ends after the top line and its two CRs; its `JSR` goes to vd_head, which prints it and then the start of the second line
; (stock or ours), and $E43D jumps to vd_banner, which prints the rest of the line, the CR, and goes on to NEW.
;
; The mailbox is not live yet when this runs (the TR's menu handler owns IO1 until the program has been loaded), so the numbers
; travel in the KERNAL image: vd_units, the two bytes after the tag "VDUNITS:" (not printed; VD_BannerUnits in firmware/IOH_VDrive.c
; searches the image for it), are written by the firmware into the image it serves, before the C64 starts: per unit (SD, USB)
; $FE = absent, 8-30 = a fixed number (U0>), $FF = automatic. An automatic unit gets the first number from 8 up that nobody answers on
; the real serial bus and that the other unit has not fixed, SD first, which is what the bus scan of the run's first disk access
; (bus_scan in vdrive.s) will give it: the probe is the same LISTEN + SECOND $6F, but through the stock routines without the mailbox
; hooks (iec_listen / iec_second / iec_unlsn). A unit with no number (absent, or every number taken) shows as "NOSD" / "NOUSB".
; The image as built says 0 / 0 (no VDrive firmware wrote it: plain Kernal Replace, an older firmware): the stock line.
;
; Code in reclaimed tape space $FB97-$FCD0 (tape-only routines: called only from tape code at $F8A1-$F8D6 and from the tape
; code vdrive.s already reclaimed). Zero page $FB-$FE: no program is running yet.

.import iec_listen, iec_second, iec_unlsn

ST    = $90
N     = $FB                 ; the number being tried for an automatic unit
RES   = $FC                 ; $FC SD, $FD USB: the number to show (8-30), anything else = none
UNIT  = $FE                 ; 0 SD, 1 USB
STROUT = $AB1E              ; BASIC: print the 0-terminated string at A/Y
CHROUT = $FFD2
NEW    = $A644
S_FREE = $E460              ; stock " BASIC BYTES FREE", CR

.segment "P_E42D"
        jsr vd_head             ; was JSR STROUT (the banner string at $E473)
.segment "P_E43D"
        jmp vd_banner           ; was LDA #<S_FREE / LDY #>S_FREE / JSR STROUT / JMP NEW
.segment "P_E49A"
        .byte 0                 ; the banner string at $E473 ends after the top line and its two CRs (was " 64K RAM SYSTEM  ", 0)

.segment "P_FB97"
; the top lines (A/Y = the string at $E473), then the start of the second line
vd_head:
        jsr STROUT
        jsr have_fw
        bne @vd
        lda #<s_ram_stock
        ldy #>s_ram_stock
        jmp STROUT
@vd:    lda #<s_ram
        ldy #>s_ram
        jmp STROUT

; Z=1 when no VDrive firmware wrote the unit bytes
have_fw:
        lda vd_units
        ora vd_units+1
        rts

; after the free bytes: the rest of the second line, CR, NEW
vd_banner:
        jsr have_fw
        bne @vd
        lda #<S_FREE            ; the stock ending, CR included
        ldy #>S_FREE
        jsr STROUT
        jmp NEW
@vd:    lda #7
        sta N
        ldx #0
@unit:  stx UNIT
        lda vd_units,x
        cmp #$FF
        bne @keep               ; fixed, or absent ($FE / 0: not 8-30, shown as "NO..")
@try:   inc N                   ; automatic: the next number that is free
        lda N
        cmp #31
        bcs @none               ; none left
        cmp vd_units            ; fixed by a unit (comparing with our own $FF is harmless)
        beq @try
        cmp vd_units+1
        beq @try
        jsr probe
        bcs @try                ; a real device answers there
        lda N
        bne @keep               ; (always: N >= 8)
@none:  lda #0
@keep:  ldx UNIT
        sta RES,x
        inx
        cpx #2
        bcc @unit

        lda #<s_vdrive
        ldy #>s_vdrive
        jsr STROUT              ; " FREE  VDRIVE "
        ldx #0
@field: stx UNIT
        lda RES,x
        cmp #8
        bcc @no
        cmp #31
        bcs @no
        pha
        jsr name
        pla
        jsr prdec2              ; "SD 8", "SD10", "USB 9", "USB11"
        jmp @sep
@no:    lda #'N'
        jsr CHROUT
        lda #'O'
        jsr CHROUT
        jsr name                ; "NOSD", "NOUSB"
@sep:   ldx UNIT
        bne @cr
        lda #' '
        jsr CHROUT
        ldx #1
        bne @field
@cr:    lda #$0D
        jsr CHROUT
        jmp NEW

; print "SD" or "USB" for UNIT
name:   ldx UNIT
        lda names,x
        ldy #>s_sd
        jmp STROUT

; C=1 when a device on the real serial bus answers at N (LISTEN N, SECOND $6F, UNLSN, as OPEN15,N,15 starts), C=0 when not.
probe:  lda #0
        sta ST
        lda N
        jsr iec_listen
        lda ST
        bmi @none               ; nothing on the bus at all
        lda #$6F
        jsr iec_second
        lda ST
        bmi @none
        jsr iec_unlsn
        sec
        rts
@none:  lda #0
        sta ST                  ; forget the DEVICE NOT PRESENT of the probe
        clc
        rts

; print A (8-30) in decimal, two wide: a space instead of a leading zero
prdec2: ldx #'0'
        sec
@ten:   sbc #10
        bcc @ones
        inx
        bcs @ten
@ones:  adc #10 + '0'           ; C=0 here
        pha
        txa
        cmp #'0'
        bne @tens
        lda #' '
@tens:  jsr CHROUT
        pla
        jmp CHROUT

s_ram_stock:
        .byte " 64K RAM SYSTEM  ", 0
s_ram:  .byte " 64K RAM  ", 0
s_vdrive:
        .byte " FREE  VDRIVE ", 0
s_sd:   .byte "SD", 0
s_usb:  .byte "USB", 0
names:  .byte <s_sd, <s_usb
.assert >s_sd = >s_usb, error, "name expects SD and USB in one page"
        .byte "VDUNITS:"        ; the tag the firmware finds vd_units by
vd_units:
        .byte 0, 0              ; written by the firmware: SD, USB ($FE absent, 8-30 fixed, $FF automatic; 0 0 = not written)
vd_banner_end:
.assert vd_banner_end <= $FCD1, lderror, "banner code overflows the reclaimed tape routines into $FCD1 (used by serial SAVE)"
