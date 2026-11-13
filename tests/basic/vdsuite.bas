0 rem vdsuite: the hardware checklist in one program (tests/basic/README.md)
5 if peek(65533)=252 then poke55,0:poke56,96:clr:rem c64: basic ends at $6000 (reutest, soak above)
10 print chr$(147);"tdrive test suite"
20 d=8:input"device";d:open15,d,15
30 dim fl$(20),sp(17),sf(17),im$(3),dn$(3),fr(3),lt(3),dh(3),sn$(17)
40 for k=1 to 3:read dn$(k),im$(k),fr(k),lt(k),dh(k):next
50 data vs64,vs64.d64,664,35,18,vs71,vs71.d71,1328,70,18,vs81,vs81.d81,3160,80,40
60 for k=1 to 8:read sn$(k):next:for k=11 to 17:read sn$(k):next
70 data setup,d64 image,d71 image,d81 image,bad sectors,card files,cmd paths,clean up
80 data bus stress,load test,ram under rom,reu,soak,status text,renumber
100 print chr$(147);"tdrive test suite, device";d
110 for k=1 to 8:print k;sn$(k):next:print" 9 run all":print" 10 checks by hand"
120 for k=11 to 17:print k;sn$(k):next:print" 0 quit"
130 input"choice";m:if m=0 then close15:sys 64738
140 if m=10 then 9000
150 if m<1 or m>17 then 100
160 ra=(m=9):sn=m:if ra then sn=1:p=0:f=0
170 print chr$(147);
200 p0=p:f0=f:print"--- ";sn$(sn):if sn>10 then on sn-10 goto 10000,11000,12000,13000,14000,15000,16000
205 on sn goto 1000,2000,2000,2000,5000,6000,7000,8000
400 sp(sn)=p-p0:sf(sn)=f-f0:print sn$(sn);":";sp(sn);"passed,";sf(sn);"failed"
410 if ra and sn<8 then sn=sn+1:goto 200
420 if ra=0 then 480
430 print:for k=1 to 8:print sn$(k);tab(13);sp(k);"passed,";sf(k);"failed":next
440 print"total";p;"passed,";f;"failed":for k=1 to f:if k<=20 then print" ";fl$(k)
450 next
480 input"return for menu";z$:goto 100
600 rem listing l$: h$ header, fb blocks free, fd=1 if n$ is listed
605 h$="":fd=0:fb=-1:hl=1:open1,d,0,l$:get#1,a$,a$
610 get#1,a$,b$:if a$+b$="" or (st and 2) then 690
620 get#1,a$,b$:nb=asc(a$+chr$(0))+256*asc(b$+chr$(0)):q$="":iq=0
630 get#1,a$:if a$="" then 660
640 if a$=chr$(34) then iq=iq+1:goto 630
650 if iq=1 then q$=q$+a$
655 goto 630
660 if hl then hl=0:h$=q$:goto 610
670 if iq=0 then fb=nb:goto 610
680 if q$=n$ then fd=1
685 goto 610
690 close1
695 if h$<>"" then if right$(h$,1)=" " then h$=left$(h$,len(h$)-1):goto 695
699 return
700 if x then print"ok   ";t$:p=p+1:return
710 print chr$(18);"fail";chr$(146);" ";t$;":";en;em$;et;es:f=f+1:if f<=20 then fl$(f)=sn$(sn)+": "+t$+str$(en)
715 return
800 open2,d,2,n$+",s,w":print#2,"x":close2:gosub 900:return
810 open2,d,2,n$+",s,r":gosub 900:close2:return
820 open2,d,2,n$:gosub 900:close2:return
850 print#15,c$:gosub 900:return
900 input#15,en,em$,et,es:return
950 rem kernal load of n$ to its own address (basic's load stops on a time-out), s = st
955 gosub 970:poke780,1:poke781,d:poke782,1:sys65466:poke780,0:sys65493:s=st:return
970 for i=1 to len(n$):poke 831+i,asc(mid$(n$,i,1)):next:poke780,len(n$):poke781,64:poke782,3:sys65469:return
1000 rem setup: the scratch folder and its three images
1010 c$="cd:/":gosub 850:t$="cd:/ to the card root":x=(en=0):gosub 700
1015 l$="$":n$="":gosub 600:t$="the root lists as "+h$:x=(left$(h$,3)="sd:" or left$(h$,4)="usb:"):gosub 700
1020 c$="md:vdscratch":gosub 850:t$="md:vdscratch (or there)":x=(en=0 or en=63):gosub 700
1030 c$="cd:vdscratch":gosub 850:t$="cd:vdscratch":x=(en=0):gosub 700
1040 for k=1 to 3:gosub 1500:t$=im$(k)+" mounted":x=(en=0):gosub 700
1050 c$="cd:_":gosub 850:next
1055 c$="cd:nowhere.d64":gosub 850:t$="missing image (62)":x=(en=62):gosub 700
1060 l$="$":n$="vs81.d81":gosub 600:t$="the folder lists them, still selected":x=fd:gosub 700
1070 goto 400
1500 rem mount im$(k) in the current folder, made with n: if missing
1510 c$="cd:"+im$(k):gosub 850:if en=0 then return
1520 print"making ";im$(k):t0=ti:print#15,"n:"+im$(k)+",vs":s=st:tm=ti-t0
1530 if s=0 and tm<=90 then 1560
1540 print"no answer in time, waiting 20 s":t0=ti
1550 if ti-t0<1200 then 1550
1560 gosub 900:print"n: took";int(tm/.6)/100;"s (limit 1.6), st";s:return
1600 rem the scratch folder, nothing mounted
1610 c$="cd:/":gosub 850:c$="md:vdscratch":gosub 850:c$="cd:vdscratch":gosub 850:return
2000 rem one image: format, seq, dos commands, blocks, rel
2010 k=sn-1:gosub 1600:gosub 1500:t$="mount "+im$(k):x=(en=0):gosub 700
2020 c$="n:"+dn$(k):gosub 850:t$="quick format":x=(en=0):gosub 700
2030 l$="$":n$="":gosub 600:t$="header "+h$+","+str$(fb)+" free":x=(h$=dn$(k) and fb=fr(k)):gosub 700
2100 open2,d,2,"vsseq,s,w":for i=1 to 100:print#2,"line,";i;",check,";i*7:next:close2
2110 open2,d,2,"vsseq,s,r":e=0:for i=1 to 100:input#2,a$,b,g$,v
2120 if a$<>"line" or b<>i or g$<>"check" or v<>i*7 then e=e+1
2130 next:close2:gosub 900:t$="seq file, 100 lines":x=(e=0 and en=0):gosub 700
2132 open2,d,2,"vsseq,s,r":e=0:for i=1 to 100:x$="line,"+str$(i)+" ,check,"+str$(i*7)+" "+chr$(13)
2134 for j=1 to len(x$):get#2,a$:if a$<>mid$(x$,j,1) then e=e+1
2136 next j,i:s=st:close2:t$="the same by get#, every byte, eoi last":x=(e=0 and s=64):gosub 700
2140 c$="s:vsseq":gosub 850:t$="scratch it (01,1)":x=(en=1 and et=1):gosub 700
2200 for i=1 to 3:n$="dt"+chr$(48+i):gosub 800:next
2210 c$="r:dtnew=dt1":gosub 850:t$="rename":x=(en=0):gosub 700
2220 n$="dt1":gosub 810:t$="old name gone (62)":x=(en=62):gosub 700
2230 n$="dtnew":gosub 810:t$="new name opens":x=(en=0):gosub 700
2240 c$="r:dt2=dtnew":gosub 850:t$="rename onto existing (63)":x=(en=63):gosub 700
2250 c$="s:dt2":gosub 850:t$="scratch one (01,1)":x=(en=1 and et=1):gosub 700
2260 c$="s:dt*":gosub 850:t$="scratch by pattern (01,2)":x=(en=1 and et=2):gosub 700
2270 n$="dt9":gosub 800:open2,d,2,"dt9,s,w":gosub 900:close2:t$="create existing (63)":x=(en=63):gosub 700
2280 c$="s:dt9":gosub 850:c$="x:foo":gosub 850:t$="unknown command (31)":x=(en=31):gosub 700
2300 open2,d,2,"d2a,s,w":print#2,"one":close2:open2,d,2,"d2b,s,w":print#2,"two":close2
2310 c$="c:d2c=d2a,d2b":gosub 850:t$="c: joins two files":x=(en=0):gosub 700
2320 open2,d,2,"d2c,s,r":input#2,a$,b$:close2:t$="joined data":x=(a$="one" and b$="two"):gosub 700
2330 c$="c:d2c=d2a":gosub 850:t$="c: onto existing (63)":x=(en=63):gosub 700
2340 c$="c:d2x=d2none":gosub 850:t$="c: missing source (62)":x=(en=62):gosub 700
2350 open2,d,2,"d2a,s,a":gosub 900:print#2,"three":close2:t$="append opens":x=(en=0):gosub 700
2360 open2,d,2,"d2a,s,r":input#2,a$,b$:close2:t$="appended data":x=(a$="one" and b$="three"):gosub 700
2370 c$="l:d2b":gosub 850:c$="s:d2b":gosub 850:t$="locked file kept":x=(en=1 and et=0):gosub 700
2380 c$="l:d2b":gosub 850:c$="s:d2b":gosub 850:t$="unlocked file scratched":x=(en=1 and et=1):gosub 700
2390 c$="w-1":gosub 850:open2,d,2,"d2w,s,w":gosub 900:close2:t$="write protect (26)":x=(en=26):gosub 700
2400 c$="w-0":gosub 850:t$="write protect off":x=(en=0):gosub 700
2410 c$="s:d2*":gosub 850:t$="scratch the rest (01,2)":x=(en=1 and et=2):gosub 700
2500 open3,d,3,"#":gosub 900:t$="buffer channel":x=(en=0):gosub 700
2510 t=lt(k):s=0
2520 c$="b-a:0"+str$(t)+str$(s):gosub 850:if en=65 and et>0 then t=et:s=es:goto 2520
2530 t$="b-a on track"+str$(t):x=(en=0):gosub 700
2540 gosub 850:t$="b-a on a used sector (65)":x=(en=65):gosub 700
2550 print#15,"b-p:3,0":for i=0 to 255:print#3,chr$(i);:next
2560 c$="u2:3,0"+str$(t)+str$(s):gosub 850:t$="u2 writes it":x=(en=0):gosub 700
2570 c$="u1:3,0"+str$(dh(k))+",0":gosub 850:get#3,a$:t$="u1 reads the header":x=(en=0 and asc(a$+chr$(0))=dh(k)):gosub 700
2580 c$="u1:3,0"+str$(t)+str$(s):gosub 850:e=0:for i=0 to 255:get#3,a$:if asc(a$+chr$(0))<>i then e=e+1
2590 if i=255 then z=st
2600 next:t$="u1 reads it back, eoi last":x=(en=0 and e=0 and (z and 64)=64):gosub 700
2610 print#15,"b-p:3,100":get#3,a$:t$="b-p":x=(asc(a$+chr$(0))=100):gosub 700
2620 c$="u1:3,0,99,0":gosub 850:t$="bad track (66)":x=(en=66):gosub 700
2630 c$="b-f:0"+str$(t)+str$(s):gosub 850:t$="b-f":x=(en=0):gosub 700
2640 close3
2700 open2,d,2,"rtrel,l,"+chr$(20):gosub 900:t$="rel file, length 20":x=(en=0):gosub 700
2710 for r=1 to 30:print#15,"p"chr$(98)chr$(r)chr$(0)chr$(1):print#2,"rec"+str$(r):next
2720 e=0:for r=30 to 1 step -1:print#15,"p"chr$(98)chr$(r)chr$(0)chr$(1):input#2,a$:if a$<>"rec"+str$(r) then e=e+1
2730 next:t$="30 records read back":x=(e=0):gosub 700
2740 print#15,"p"chr$(98)chr$(100)chr$(0)chr$(1):gosub 900:t$="record past the end (50)":x=(en=50):gosub 700
2750 print#2,"far":print#15,"p"chr$(98)chr$(100)chr$(0)chr$(1):input#2,a$:t$="a write grows it":x=(a$="far"):gosub 700
2760 print#15,"p"chr$(98)chr$(1)chr$(0)chr$(1):print#2,"123456789012345678901234":gosub 900:t$="overflow (51)":x=(en=51):gosub 700
2770 close2:open2,d,2,"rtrel":print#15,"p"chr$(98)chr$(30)chr$(0)chr$(1):input#2,a$:t$="kept after close":x=(a$="rec 30"):gosub 700
2780 close2:c$="c:rtcopy=rtrel":gosub 850:t$="c: of a rel file":x=(en=0):gosub 700
2790 c$="s:rt*":gosub 850:t$="scratch both (01,2)":x=(en=1 and et=2):gosub 700
2900 l$="$":n$="":gosub 600:t$="blocks free back to"+str$(fr(k)):x=(fb=fr(k)):gosub 700
2910 c$="v":gosub 850:gosub 600:t$="validate keeps them":x=(en=0 and fb=fr(k)):gosub 700
2920 c$="cd:_":gosub 850:goto 400
5000 rem bad sectors: vdbad.d64 in the card root
5010 c$="cd:/":gosub 850:c$="cd:vdbad.d64":gosub 850:t$="mount vdbad.d64":x=(en=0):gosub 700
5020 if en then 400
5030 open3,d,3,"#":c$="u1:3,0,30,5":gosub 850:t$="u1 on a bad sector (21,30,5)":x=(en=21 and et=30 and es=5):gosub 700
5040 c$="u1:3,0,30,4":gosub 850:t$="u1 on a good one":x=(en=0):gosub 700
5050 close3:open2,d,2,"badfile":n=0
5060 get#2,a$:s=st:if (s and 2)=0 then n=n+1:if s=0 and n<1100 then 5060
5070 gosub 900:close2:t$="read stops after 254 bytes ("+mid$(str$(n),2)+", st"+str$(s)+")":x=(n=254 and (s and 2)=2):gosub 700
5080 t$="status names the sector (23)":x=(en=23 and et=@BT@ and es=@BS@):gosub 700
5090 for i=0 to 1023:poke 49152+i,0:next:n$="badfile":gosub 950
5200 gosub 900:e=0:for i=0 to 251:if peek(49152+i)<>1+i-int(i/250)*250 then e=e+1
5210 next:for i=252 to 511:if peek(49152+i) then e=e+1
5220 next:t$="load stops at the bad sector, st"+str$(s):x=(e=0 and (s and 2)=2):gosub 700
5230 t$="status after the load (23)":x=(en=23):gosub 700
5240 c$="cd:/":gosub 850:goto 400
6000 rem card files with nothing mounted: vdplain.prg in the root, md/rd in vdscratch
6010 c$="cd:/":gosub 850:open2,d,2,"vdplain.prg":gosub 900:n=0:e=0
6020 get#2,a$:s=st:if n>1 then j=n-2:if asc(a$+chr$(0))<>(j*7+3)-int((j*7+3)/256)*256 then e=e+1
6030 n=n+1:if s=0 and n<700 then 6020
6040 close2:t$="plain file opens":x=(en=0):gosub 700
6050 t$="602 bytes, eoi last":x=(n=602 and e=0 and (s and 64)=64):gosub 700
6060 n$="vdplain":gosub 820:t$="name without .prg":x=(en=0):gosub 700
6070 n$="vdpl*":gosub 820:t$="pattern":x=(en=0):gosub 700
6080 n$="nofile.prg":gosub 820:t$="missing file (62)":x=(en=62):gosub 700
6090 open2,d,2,"vsx,s,w":gosub 900:close2:t$="no writing (74)":x=(en=74):gosub 700
6100 gosub 1600:n$="/vdplain.prg":gosub 820:t$="path from the root":x=(en=0):gosub 700
6110 l$="$":n$="vdscratch":gosub 600:t$="its folder stays selected":x=fd:gosub 700
6200 gosub 1600:k=1:gosub 1500:c$="cd:_":gosub 850
6210 c$="md:t1":gosub 850:t$="md:t1":x=(en=0):gosub 700
6220 l$="$":n$="t1":gosub 600:t$="t1 listed":x=fd:gosub 700
6230 gosub 850:t$="md:t1 again (63)":x=(en=63):gosub 700
6240 c$="rd:t1":gosub 850:t$="rd:t1":x=(en=0):gosub 700
6250 gosub 600:t$="t1 gone":x=(fd=0):gosub 700
6260 gosub 850:t$="rd:t1 again (62)":x=(en=62):gosub 700
6270 c$="md:a*b":gosub 850:t$="bad name (33)":x=(en=33):gosub 700
6280 c$="md:":gosub 850:t$="no name (34)":x=(en=34):gosub 700
6290 c$="rd:vs64.d64":gosub 850:t$="rd on a file (62)":x=(en=62):gosub 700
6300 c$="cd:vs64.d64":gosub 850:c$="md:t1":gosub 850:t$="md with an image mounted (31)":x=(en=31):gosub 700
6310 c$="cd:_":gosub 850:c$="md:t2":gosub 850:c$="cd:t2":gosub 850:c$="s:e.d64":gosub 850:c$="n:e.d64,ee":gosub 850
6320 c$="cd:_":gosub 850:c$="cd:..":gosub 850:c$="rd:t2":gosub 850:t$="rd, folder not empty (63)":x=(en=63):gosub 700
6330 c$="cd:/":gosub 850:for i=0 to 1023:poke 49152+i,0:next:n$="vdplain.prg":gosub 950
6500 e=0:for j=0 to 599:if peek(49152+j)<>(j*7+3)-int((j*7+3)/256)*256 then e=e+1
6510 next:t$="load of a plain file, st"+str$(s):x=(e=0 and peek(49752)=0 and (s and 191)=0):gosub 700
6520 goto 400
7000 rem cmd hd path syntax, from the root and in vdscratch
7010 gosub 1600:k=1:gosub 1500:c$="s:vskeep":gosub 850:open2,d,2,"vskeep,s,w":print#2,"keep":close2:c$="cd:_":gosub 850
7020 c$="cd//":gosub 850:t$="cd// (the root)":x=(en=0):gosub 700
7030 l$="$":n$="vdscratch":gosub 600:t$="root listed":x=fd:gosub 700
7040 c$="cd//vdscratch":gosub 850:t$="cd//vdscratch":x=(en=0):gosub 700
7050 n$="vs64.d64":gosub 600:t$="vdscratch listed":x=fd:gosub 700
7060 c$="cd/":gosub 850:gosub 600:t$="cd/ stays":x=(en=0 and fd):gosub 700
7070 c$="md//vdscratch/:cm":gosub 850:t$="md//vdscratch/:cm":x=(en=0):gosub 700
7080 c$="cd/cm":gosub 850:gosub 600:t$="cd/cm (relative)":x=(en=0 and fd=0):gosub 700
7090 c$="cd//vdscratch":gosub 850:c$="rd/cm":gosub 850:t$="rd/cm":x=(en=0):gosub 700
7100 n$="cm":gosub 600:t$="cm gone":x=(fd=0):gosub 700
7110 c$="cd//":gosub 850:a$="":open2,d,2,"//vdscratch/vs64.d64/:vskeep,s,r":gosub 900:if en=0 then input#2,a$
7120 close2:t$="a file by cmd path":x=(a$="keep"):gosub 700
7130 c$="cd:/":gosub 850:l$="$//vdscratch/:vs*":n$="vs64.d64":gosub 600:t$="$//vdscratch/:vs*":x=fd:gosub 700
7140 l$="$":n$="vdscratch":gosub 600:t$="still at the root":x=fd:gosub 700
7150 c$="n//vdscratch/:vscmd.d64,cm":gosub 850:t$="n// (63: made before)":x=(en=0 or en=63):gosub 700
7160 c$="cd:/":gosub 850:goto 400
8000 rem clean up: the scratch images deleted with s:, the folders with rd:
8010 gosub 1600:c$="s:t2/e.d64":gosub 850:t$="s: an image in a folder":x=(en=1):gosub 700
8020 c$="s:nofile.d64":gosub 850:t$="a missing image counts 0":x=(en=1 and et=0):gosub 700
8030 c$="s:vs*.d64":gosub 850:t$="no patterns (33)":x=(en=33):gosub 700
8040 c$="s:/vdplain.prg":gosub 850:t$="images only (64)":x=(en=64):gosub 700
8050 c$="s//vdscratch/:vscmd.d64":gosub 850:t$="s// cmd path":x=(en=1):gosub 700
8060 c$="s:vs64.d64,vs71.d71,vs81.d81":gosub 850:t$="the three images":x=(en=1):gosub 700
8070 l$="$":n$="vs64.d64":gosub 600:t$="images gone":x=(fd=0 and fb>=0):gosub 700
8080 c$="rd:t2":gosub 850:t$="rd:t2":x=(en=0 or en=62):gosub 700
8090 c$="cd:/":gosub 850:c$="rd:vdscratch":gosub 850:t$="rd:vdscratch":x=(en=0):gosub 700
8100 n$="vdscratch":gosub 600:t$="the root without it":x=(fd=0):gosub 700
8110 goto 400
9000 print chr$(147);"checks by hand (see tests/basic/readme)"
9010 print"- 16 status text: run it on a 1541 too":print"  (device 8 there), compare the texts"
9015 print"- with vdtest.d64 mounted:"
9020 print"- run/stop during load"chr$(34)"big"chr$(34)",8,1:":print"  break, then load"chr$(34)"$"chr$(34)",8 works"
9030 print"- load"chr$(34)"hello"chr$(34)" with no device number"
9040 print"- tdrive+reu: reu-checker says 512 kb":print"  with an image mounted"
9050 print"- vdcopy: sd to 1541 and back"
9055 print"- s: on an image made read-only on a":print"  pc: 26, the file stays"
9060 print"- d81 time: setup prints it the first":print"  time it makes vs81.d81"
9070 print"- dmatest (on vdtest.d64) with":print"  tools/dmatest.py on the host"
9090 input"return for menu";z$:goto 100
9500 rem mc = the jump table of the machine code after this program (tests/asm/suite.s), 0 if missing
9510 mc=peek(45)+256*peek(46)-12:if peek(65533)<>252 then mc=0:print"c64 only":return
9520 if peek(mc-2)<>86 or peek(mc-1)<>68 then mc=0:print"machine code missing: load vdsuite again"
9530 return
10000 rem bus stress (bustest): mailbox registers and window at full speed, no commands; about a minute
10010 gosub 9500:if mc=0 then 400
10020 sys mc:e=peek(820)+256*peek(821):t$="no wrong read ("+mid$(str$(e),2)+")":x=(e=0 and peek(822)=0):gosub 700:goto 400
11000 rem load test (loadtest): lda000 and ldd000 from vdtest.d64 in the card root, under basic and across $d000
11010 gosub 9500:if mc=0 then 400
11020 c$="cd:/":gosub 850:c$="cd:vdtest.d64":gosub 850:t$="mount vdtest.d64 (card root)":x=(en=0):gosub 700:if en then 400
11030 poke 186,d:sys mc+3:t$="every byte, the guards, the end":x=(peek(820)=0):gosub 700:c$="cd:/":gosub 850:goto 400
12000 rem ram under rom (vaulttest): $d000-$feff with $01=$34, as quest for the king uses it
12010 gosub 9500:if mc=0 then 400
12020 sys mc+6:e=0:for i=0 to 5:e=e+peek(820+i):next:t$="no wrong byte":x=(e=0):gosub 700:goto 400
13000 rem reu (reutest): its size by bank wrap and every byte of it
13010 gosub 9500:if mc=0 then 400
13020 sys mc+9:b=peek(821):t$="an reu,"+str$(b*64)+" kb":x=(b>0):gosub 700
13030 t$="every byte kept":x=(b>0 and peek(822)=0 and peek(823)=0):gosub 700:goto 400
14000 rem soak: a 4 kb file saved once on vs64.d64, then loaded and checked until a key is pressed
14010 if peek(65533)<>252 then print"c64 only":goto 400
14020 gosub 1600:k=1:gosub 1500:c$="s:vssoak":gosub 850:print"making the file"
14030 for i=0 to 4095:poke 24576+i,(i and 255)+int(i/256)*7 and 255:next
14040 n$="vssoak":gosub 970:poke780,1:poke781,d:poke782,0:sys65466
14050 poke251,0:poke252,96:poke780,251:poke781,0:poke782,112:sys65496:gosub 900:t$="save 4 kb":x=(en=0):gosub 700
14060 n=0:e=0:print"any key stops it"
14070 for i=0 to 4095 step 8:poke 24576+i,0:next:n$="vssoak":gosub 950:n=n+1
14080 for i=0 to 4095 step 8:if peek(24576+i)<>((i and 255)+int(i/256)*7 and 255) then e=e+1
14090 next:if s and 191 then e=e+1
14100 print"pass";n;"errors";e:get k$:if k$="" then 14070
14110 c$="s:vssoak":gosub 850:c$="cd:_":gosub 850:t$="soak,"+str$(n)+" loads":x=(e=0):gosub 700:goto 400
15000 rem status text: the raw error channel text of a few commands; run it on a real drive too and compare
15010 c$="s:nofile":gosub 15100:c$="r:a=zzz":gosub 15100:c$="x:foo":gosub 15100
15020 open2,d,2,"nofile,s,r":gosub 15110:close2:c$="i":gosub 15100:goto 400
15100 print#15,c$
15110 print"[";:c=0
15120 get#15,a$:c=c+1:if a$=chr$(13) or c>60 then print"]":return
15130 print a$;:goto 15120
16000 rem renumber with u0>: the new number answers and the suite goes on with it (17 again puts it back)
16010 n=0:input"new device (8-30)";n:if n<8 or n>30 then 400
16020 print#15,"u0>"+chr$(n):close15:d=n:open15,d,15:gosub 900:print"new device";d;"answers:";en;em$
16030 t$="device"+str$(d)+" answers":x=(en=0):gosub 700
16040 print"a fixed number now, kept in eeprom;":print"u0>"chr$(34)"+chr$(0) is automatic again":goto 400
