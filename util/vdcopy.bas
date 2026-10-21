10 print chr$(147);"vdcopy - copy files from a real drive to a vdrive unit"
20 dim n$(254),t$(254),b(254),m(254):z$=chr$(0)
30 for i=0 to @MCLEN@-1:read x:poke 828+i,x:next
40 rem the first bus access also runs the vdrive scan that numbers the units
50 u=8:gosub 900
60 du=peek(56854):if du<8 or du>30 then du=9
70 su=8:if su=du then su=9
80 print"copy from unit (";mid$(str$(su),2);")":input su
90 print"copy to unit (";mid$(str$(du),2);")":input du
100 if su<8 or su>30 or du<8 or du>30 or su=du then print"bad unit numbers":goto 80
110 u=su:gosub 900:if pr<>0 then print"nothing on unit";su:goto 80
120 u=du:gosub 900:if pr<>0 then print"nothing on unit";du:goto 80
130 open15,su,15:open14,du,15
140 print"reading directory of unit";su:gosub 300
150 if nf=0 then print"no files to copy":goto 860
160 gosub 500
170 print:print"copy: a=all, or 1,3,5-8 (return=quit)"
180 gosub 5000:s$=i$:print:if s$="" then 860
190 gosub 6000:if nm=0 then print"nothing selected":goto 170
200 print nm;"selected. replace existing (y/n)?";
210 get k$:if k$<>"y" and k$<>"n" then 210
220 ow=(k$="y"):print k$:print
230 nc=0:nx=0:nr=0:ab=0:poke809,3
240 for i=0 to nf-1:if m(i)=0 then 260
250 gosub 600:if ab then 270
260 next
270 poke809,246
280 print:print nc;"copied,";nx;"skipped,";nr;"failed"
290 goto 170
300 rem read the directory of unit su
310 nf=0:sk=0:hd=0:h$=""
320 open1,su,0,"$":poke144,0
330 get#1,a$,a$
340 get#1,a$,b$:if (st and 64) or a$+b$="" then 480
350 if st and 191 then print"directory read error":nf=0:goto 480
360 get#1,a$,b$:bl=asc(a$+z$)+256*asc(b$+z$):l$=""
370 get#1,a$:if a$<>"" then l$=l$+a$:goto 370
380 if hd=0 then hd=1:h$=l$:goto 340
390 q1=0:q2=0:for k=1 to len(l$):if mid$(l$,k,1)<>chr$(34) then 410
400 if q1=0 then q1=k
405 q2=k
410 next
420 if q2<=q1 then 340
430 y$="":for k=q2+1 to len(l$):c=asc(mid$(l$,k,1)+z$):if c>=65 and c<=90 then y$=mid$(l$,k,1):k=len(l$)
440 next
450 if (y$<>"p" and y$<>"s" and y$<>"u") or nf>254 then sk=sk+1:goto 340
460 n$(nf)=mid$(l$,q1+1,q2-q1-1):t$(nf)=y$:b(nf)=bl:nf=nf+1:goto 340
480 close1:return
500 rem list the files, 20 lines at a time
510 print chr$(147);h$:print nf;"files";
520 if sk>0 then print", ";sk;"not listed (rel, del or too many)";
530 print:ln=3
540 for i=0 to nf-1
550 print right$("   "+str$(i+1),3);" ";chr$(34);n$(i);chr$(34);left$("                ",16-len(n$(i)));" ";t$(i);right$("    "+str$(b(i)),4)
560 ln=ln+1:if ln<23 or i=nf-1 then 590
570 print"-- press a key --";
580 get k$:if k$="" then 580
585 print:ln=0
590 next:return
600 rem copy file i from unit su to unit du
610 f$=n$(i):y$=t$(i):bd=0
620 for k=1 to len(f$):c=asc(mid$(f$,k,1)):if c=42 or c=44 or c=58 or c=61 or c=63 then bd=1
625 next
630 print right$("   "+str$(i+1),3);" ";chr$(34);f$;chr$(34);" ";
635 if bd then print"skipped (odd name)":nx=nx+1:return
640 open2,su,2,f$+","+y$+",r":gosub 870:if en=0 then 660
650 close2:print"read error";en:nr=nr+1:return
660 open3,du,3,f$+","+y$+",w":gosub 880
670 if en=63 and ow then close3:print#14,"s:"+f$:gosub 880:open3,du,3,f$+","+y$+",w":gosub 880
680 if en=0 then 700
690 close3:close2:if en=63 then print"exists":nx=nx+1:return
695 print"write error";en;em$:nr=nr+1:return
700 bt=0
710 sys828
720 n=peek(1008)+256*peek(1009):rs=peek(1010):ws=peek(1011):ep=peek(1012)
730 bt=bt+n:print".";:gosub 880
740 if ep<>0 or ws<>0 or (rs and 191)<>0 or en<>0 then 780
750 if (rs and 64)<>0 then 790
760 if peek(145)=127 then ab=1:m$="stopped":goto 800
770 goto 710
780 m$="error":if ep<>0 then m$="channel error"+str$(ep)
781 if ws<>0 then m$="write failed, st"+str$(ws)
782 if (rs and 191)<>0 then m$="read failed, st"+str$(rs)
783 if en<>0 then m$=str$(en)+" "+em$
785 goto 800
790 close3:close2:gosub 880:if en=0 then print" ok";bt;"bytes":nc=nc+1:return
795 m$=str$(en)+" "+em$
800 close3:close2:print#14,"s:"+f$:gosub 880:print" ";m$:nr=nr+1:return
860 close14:close15:poke809,246:end
870 input#15,en,em$,et,es:return
880 input#14,en,em$,et,es:return
900 poke780,u:sys65457:poke780,111:sys65427:pr=peek(144)
910 if pr=0 then sys65454
920 poke144,0:return
5000 rem line input into i$ (digits , - and a only)
5010 i$=""
5020 print chr$(18);" ";chr$(146);chr$(157);
5030 get k$:if k$="" then 5030
5040 print" ";chr$(157);
5050 if k$=chr$(13) then return
5060 if k$=chr$(20) then if len(i$)>0 then i$=left$(i$,len(i$)-1):print chr$(20);
5065 if k$=chr$(20) then 5020
5070 c=asc(k$):if len(i$)>30 then 5020
5080 if (c>=48 and c<=57) or c=44 or c=45 or c=65 or c=193 then i$=i$+k$:print k$;
5090 goto 5020
6000 rem s$ -> m(), nm
6010 for k=0 to nf-1:m(k)=0:next:nm=0
6020 c$=left$(s$,1):if c$="a" or c$=chr$(193) then for k=0 to nf-1:m(k)=1:next:nm=nf:return
6030 v=0:lo=0:rg=0
6040 for k=1 to len(s$)+1
6050 c=asc(mid$(s$+",",k,1))
6060 if c>=48 and c<=57 then v=v*10+c-48:goto 6130
6070 if c=45 then lo=v:v=0:rg=1:goto 6130
6080 hi=v:if rg=0 then lo=v
6085 if rg=1 and v=0 then hi=nf
6090 if lo<1 or lo>nf then 6120
6095 if hi>nf then hi=nf
6100 if hi<lo then 6120
6105 for j=lo to hi:if m(j-1)=0 then m(j-1)=1:nm=nm+1
6110 next
6120 v=0:lo=0:rg=0
6130 next:return
10000 rem machine code for SYS 828 (generated from util/vdcopy.s)
@MCDATA@
