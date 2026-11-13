;TDrive: with a TDrive Special IO and a virtual drive without a fixed number, the PreStart before this holds the replacement
;KERNAL back and asks (rwRegScratch nonzero) for a probe of the serial bus, done here with the C64's own KERNAL, so the
;program finds the right numbers in $DE16/$DE17 from its first instruction. Each number from 8 up that no drive answers
;is offered with another PreStart (rwRegScratch = the number); the answer is how many drives still wait. $ff: none left.
;(teensyrom-vdrive firmware/tdrive_probe.asm, inserted into MainMenu.asm by tools/make_tr_patch.sh; docs/DESIGN.md 37)
TDriveProbe:
   lda rwRegScratch+IO1Port
   beq TDriveProbeDone ;nothing to probe
   lda #8
   sta smcTDriveProbeDev+1
TDriveProbeNext:
   lda smcTDriveProbeDev+1
   cmp #31
   bcs TDriveProbeOut
   lda #0
   sta $90 ;ST
smcTDriveProbeDev
   lda #8 ;the number probed (self modified code)
   jsr $ffb1 ;LISTEN
   lda $90
   bmi TDriveProbeFree ;nothing on the bus at all
   lda #$6f
   jsr $ff93 ;SECOND (command channel, nothing is sent on it)
   lda $90
   bmi TDriveProbeFree
   jsr $ffae ;UNLSN: a real drive answers at this number
   inc smcTDriveProbeDev+1
   jmp TDriveProbeNext
TDriveProbeOut:
   lda #$ff ;8-30 all taken
   bne TDriveProbeTell
TDriveProbeFree:
   lda smcTDriveProbeDev+1
   inc smcTDriveProbeDev+1
TDriveProbeTell:
   sta rwRegScratch+IO1Port
   lda #rCtlKERNALPreStartWAIT
   sta wRegControl+IO1Port
   jsr WaitForTRDots
   lda rwRegScratch+IO1Port
   bne TDriveProbeNext ;a drive still waits for a number
TDriveProbeDone:
   rts

