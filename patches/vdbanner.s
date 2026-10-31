; Power-on banner with the virtual drives' device numbers. The top line stays stock; the second becomes
;     64K RAM, 38911 FREE, VDRIVE 8+9
; (", VDRIVE ..." only for the units that are there: "VDRIVE 9" with only the SD card, "VDRIVE N/A" with neither, nothing at all
; without the VDrive firmware).
;
; BASIC's start-up message routine (INITMS, $E422, in the KERNAL chip) prints the banner string at $E473 (up to " 64K RAM"),
; the free bytes ($BDCD), the string at $E460 and ends in `JMP $A644` (NEW). We shorten both strings and point that JMP at
; vd_banner, which prints the drives, the CR, then goes on to NEW.
;
; The mailbox is not live yet when this runs (the TR's menu handler owns IO1 until the program has been loaded), so the numbers
; travel in the KERNAL image: vd_units, the two bytes after the ", VDRIVE " string (that string with its 0 is the tag the firmware
; looks for, VD_BannerUnits in firmware/IOH_VDrive.c), are written by the firmware into the image it serves, before the C64 starts:
; per unit (SD, USB) $FE = absent, 8-30 = a fixed number (U0>), $FF = automatic. An automatic unit gets the first number from 8 up that
; nobody answers on the real serial bus and that the other unit has not fixed, SD first, which is what the bus scan of the run's
; first disk access (bus_scan in vdrive.s) will give it: the probe is the same LISTEN + SECOND $6F, but through the stock routines
; without the mailbox hooks (iec_listen / iec_second / iec_unlsn). No number to show (both absent, or every number taken): "N/A".
; The image as built says 0 / 0 (no VDrive firmware wrote it: plain Kernal Replace, an older firmware): no VDrive text at all.
;
; Code in reclaimed tape space $FB97-$FCD0 (tape-only routines: called only from tape code at $F8A1-$F8D6 and from the tape
; code vdrive.s already reclaimed). Zero page $FB-$FD: no program is running yet.

.import iec_listen, iec_second, iec_unlsn

ST    = $90
N     = $FB                 ; the number being tried for an automatic unit
SEP   = $FC                 ; 0 until ", VDRIVE " has been printed, then "+"
UNIT  = $FD                 ; 0 SD, 1 USB
STROUT = $AB1E              ; BASIC: print the 0-terminated string at A/Y
CHROUT = $FFD2
NEW    = $A644

.segment "P_E444"
        jmp vd_banner           ; was JMP NEW
.segment "P_E460"
        .byte " FREE", 0        ; was " BASIC BYTES FREE", CR (the CR comes after the drives now)
.segment "P_E49A"
        .byte " 64K RAM, ", 0   ; the end of the banner string at $E473, was " 64K RAM SYSTEM  " (the number follows with no space)

.segment "P_FB97"
vd_banner:
        lda #7
        sta N
        lda #0
        sta SEP
        sta UNIT
@unit:  ldx UNIT
        lda vd_units,x
        beq @next               ; no information (0)
        cmp #$FE
        beq @next               ; absent
        cmp #$FF
        bne @show               ; a fixed number
@try:   inc N                   ; automatic: the next number that is free
        lda N
        cmp #31
        bcs @next               ; none left
        cmp vd_units            ; fixed by a unit (comparing with our own $FF is harmless)
        beq @try
        cmp vd_units+1
        beq @try
        jsr probe
        bcs @try                ; a real device answers there
        lda N
@show:  pha
        lda SEP
        bne @plus
        lda #'+'
        sta SEP
        lda #<s_vdrive
        ldy #>s_vdrive
        jsr STROUT
        jmp @num
@plus:  jsr CHROUT
@num:   pla
        jsr prdec
@next:  inc UNIT
        lda UNIT
        cmp #2
        bcc @unit
        lda SEP
        bne @cr                 ; a number was printed
        lda vd_units
        ora vd_units+1
        beq @cr                 ; no VDrive firmware: no text
        lda #<s_vdrive
        ldy #>s_vdrive
        jsr STROUT
        lda #<s_na
        ldy #>s_na
        jsr STROUT              ; ", VDRIVE N/A": neither unit has a number
@cr:    lda #$0D
        jsr CHROUT
        jmp NEW

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

; print A (0-39) in decimal, no leading space or zero
prdec:  ldx #'0'
        sec
@ten:   sbc #10
        bcc @ones
        inx
        bcs @ten
@ones:  adc #10 + '0'           ; C=0 here
        pha
        txa
        cmp #'0'
        beq @one
        jsr CHROUT
@one:   pla
        jmp CHROUT

s_na:   .byte "N/A", 0
s_vdrive:
        .byte ", VDRIVE ", 0    ; also the tag the firmware finds vd_units by
vd_units:
        .byte 0, 0              ; written by the firmware: SD, USB ($FE absent, 8-30 fixed, $FF automatic; 0 0 = not written)
vd_banner_end:
.assert vd_units = s_vdrive + 10, error, "the firmware expects vd_units right after the tag"
.assert vd_banner_end <= $FCD1, lderror, "banner code overflows the reclaimed tape routines into $FCD1 (used by serial SAVE)"
