10 rem vdrive config: edit the alt-button disk lists, show and change the mounted images
20 gosub 9000
30 if d0=0 and d1=0 then 9500
40 un=0:if d0=0 then un=1
50 gosub 880:gosub 3000:gosub 1000:gosub 1200:cu=0:tp=0:gosub 2500
60 m$="ready":gosub 7000
100 get k$:if k$="" then 100
110 k=asc(k$)
120 if k=145 then gosub 4000:goto 100
130 if k=17 then gosub 4050:goto 100
140 if k=13 then gosub 4100:goto 100
150 if k=66 then gosub 4150:goto 100
160 if k=85 then gosub 4200:goto 100
170 if k=65 then gosub 4300:goto 100
180 if k=69 then gosub 4400:goto 100
190 if k=68 then gosub 4500:goto 100
200 if k=43 then gosub 4600:goto 100
210 if k=45 then gosub 4650:goto 100
220 if k=84 then gosub 4700:goto 100
230 if k=82 then gosub 4800:goto 100
240 if k=81 then 9600
250 goto 100
800 input#el,en,er$,et,es:return
810 print#el,cm$:goto 800
880 dv=d0:el=15:if un=1 then dv=d1:el=14
890 return
1000 nl=0:open2,dv,2,"!config":gosub 800
1010 if en<>0 then close2:return
1020 ln$=""
1030 get#2,a$:s=st
1040 if a$=chr$(13) then gosub 1100:ln$="":goto 1070
1050 if a$<>"" then ln$=ln$+a$
1070 if (s and 64)=0 then 1030
1080 if ln$<>"" then gosub 1100
1090 close2:return
1100 if nl<119 then l$(nl)=ln$:nl=nl+1
1110 return
1200 ne=0:if nl=0 then return
1210 for i=0 to nl-1
1220 p$=l$(i)
1225 if left$(p$,1)=" " then p$=mid$(p$,2):goto 1225
1230 if left$(p$,1)<>"/" or ne>=32 then 1260
1240 gosub 1300
1250 ep$(ne)=pa$:ec(ne)=pc:ef(ne)=pf:ex(ne)=i:ne=ne+1
1260 next:return
1300 pa$=p$:pc=1:pf=1
1310 gosub 1400:if ok=0 then 1390
1320 n1=v:gosub 1400:if ok=0 then pc=n1 and 15:goto 1390
1330 pc=v and 15:pf=n1:if pf>20 then pf=20
1390 if right$(pa$,1)=" " then pa$=left$(pa$,len(pa$)-1):goto 1390
1395 return
1400 ok=0:i2=len(pa$)
1410 if i2<1 then return
1420 if mid$(pa$,i2,1)="," then 1440
1430 i2=i2-1:goto 1410
1440 t$=mid$(pa$,i2+1)
1450 if left$(t$,1)=" " then t$=mid$(t$,2):goto 1450
1460 if right$(t$,1)=" " then t$=left$(t$,len(t$)-1):goto 1460
1470 if t$="" then return
1472 if len(t$)>3 then return
1475 j=1
1480 if j>len(t$) then 1495
1485 c=asc(mid$(t$,j,1)):if c<48 or c>57 then return
1490 j=j+1:goto 1480
1495 v=val(t$):pa$=left$(pa$,i2-1):ok=1:return
1500 t=0:if nl=0 then 1520
1505 for i=0 to nl-1:t=t+len(l$(i))+1:next
1520 if t>2000 then m$="list too long to save":gosub 7000:return
1530 open2,dv,2,"!config,w":gosub 800
1540 if en<>0 then close2:m$="cannot write:"+er$:gosub 7000:return
1550 if nl=0 then 1570
1555 for i=0 to nl-1:print#2,l$(i):next
1570 close2:gosub 800
1580 m$="saved":if en<>0 then m$="write error"+str$(en)
1590 gosub 7000:return
1700 ln$=""
1710 get#2,a$:s=st:if a$=chr$(13) then return
1720 if a$<>"" then ln$=ln$+a$
1730 if (s and 64)=0 then 1710
1740 return
2000 rw=4+kk-tp:cl=0:gosub 7500
2010 if kk>=ne then print left$(sp$,39);:return
2020 a$=" ":if kk=cu then a$=">"
2030 b$=" ":if ep$(kk)=mp$(un) then b$="*"
2040 n$=right$("  "+str$(kk+1),3)
2050 p$=ep$(kk):if len(p$)>22 then p$=".."+right$(p$,20)
2060 p$=left$(p$+sp$,22)
2070 print a$;b$;n$;" ";
2080 if kk=cu then print chr$(18);
2090 print p$;chr$(146);" ";right$("  "+str$(ec(kk)),3);" ";chr$(18);chr$(cc(ec(kk)));"  ";chr$(146);chr$(5);
2100 print right$("  "+str$(ef(kk)),3);"x";:return
2400 for r=0 to vr-1:kk=tp+r:gosub 2000:next:return
2500 print chr$(147);chr$(18);left$(" vdrive config"+sp$,39);chr$(146);
2550 gosub 2600:gosub 2700:gosub 2400:gosub 2900:return
2600 for u=0 to 1
2610 rw=1+u:cl=0:gosub 7500
2620 x=d0:if u=1 then x=d1
2630 hd$=nm$(u)+str$(x)+": ":a$="no drive"
2640 if x=0 then hd$=nm$(u)+"  : "
2650 if x<>0 then a$=mp$(u):if a$="" then a$="(nothing mounted)"
2660 w=39-len(hd$)
2670 if len(a$)>w then a$=".."+right$(a$,w-2)
2680 if u=un then print chr$(18);
2690 print hd$;chr$(146);left$(a$+sp$,w);:next:return
2700 rw=3:cl=0:gosub 7500
2710 print chr$(18);left$("list: "+sn$(un)+" ("+mid$(str$(ne),2)+" entries)"+sp$,39);chr$(146);:return
2900 rw=21:cl=0:gosub 7500:print left$("return:mount b:browse+mount u:unmount"+sp$,39);
2910 rw=22:cl=0:gosub 7500:print left$("a:add e:edit d:delete +/-:reorder"+sp$,39);
2920 rw=23:cl=0:gosub 7500:print left$("t:sd/usb list r:refresh q:quit"+sp$,39);:return
2950 if cu<tp then tp=cu
2960 if cu>=tp+vr then tp=cu-vr+1
2970 return
3000 for u=0 to 1
3010 mp$(u)="":x=d0:y=15:if u=1 then x=d1:y=14
3020 if x=0 then 3090
3030 open2,x,2,"!state":input#y,en,er$,et,es
3040 if en<>0 then close2:goto 3090
3050 gosub 1700:mp$(u)=ln$:close2
3090 next:return
4000 if cu=0 then return
4010 cu=cu-1:if cu<tp then tp=cu:gosub 2400:return
4020 kk=cu+1:gosub 2000:kk=cu:gosub 2000:return
4050 if cu>=ne-1 then return
4060 cu=cu+1:if cu>=tp+vr then tp=cu-vr+1:gosub 2400:return
4070 kk=cu-1:gosub 2000:kk=cu:gosub 2000:return
4100 if ne=0 then return
4105 pa$=ep$(cu)
4120 cm$="cd:"+pa$:gosub 810
4122 m$="mounted":if en<>0 then m$="error"+str$(en)+","+er$
4124 gosub 3000:gosub 2550:gosub 7000:return
4150 gosub 5000:if br=0 then gosub 2500:return
4155 pa$=bs$:goto 4120
4200 if mp$(un)="" then m$="nothing mounted":gosub 7000:return
4210 cm$="cd:_":gosub 810:m$="unmounted":if en<>0 then m$="error"+str$(en)
4220 gosub 3000:gosub 2550:gosub 7000:return
4300 if ne>=32 or nl>=118 then m$="list is full":gosub 7000:return
4310 gosub 5000:if br=0 then gosub 2500:return
4320 pa$=bs$:lb$="colour 0-15: ":in$="1":ml=2:gosub 7100:if cn=1 then gosub 2500:return
4330 pc=val(in$):if pc<0 or pc>15 then pc=1
4340 lb$="flashes 0-20: ":in$="1":ml=2:gosub 7100:if cn=1 then gosub 2500:return
4350 pf=val(in$):if pf<0 or pf>20 then pf=1
4360 l$(nl)=pa$+", "+mid$(str$(pc),2)+", "+mid$(str$(pf),2):nl=nl+1
4370 gosub 1200:cu=ne-1:gosub 2950:gosub 2500:gosub 1500:return
4400 if ne=0 then return
4410 lb$="path(?=browse): ":in$=ep$(cu):ml=78:gosub 7100:if cn=1 then gosub 2550:return
4420 pa$=in$:if pa$<>"?" then 4440
4430 gosub 5000:if br=0 then gosub 2500:return
4435 pa$=bs$
4440 if left$(pa$,1)<>"/" then m$="path must start with /":gosub 2550:gosub 7000:return
4445 lb$="colour 0-15: ":in$=mid$(str$(ec(cu)),2):ml=2:gosub 7100:if cn=1 then gosub 2500:return
4450 pc=val(in$):if pc<0 or pc>15 then pc=ec(cu)
4455 lb$="flashes 0-20: ":in$=mid$(str$(ef(cu)),2):ml=2:gosub 7100:if cn=1 then gosub 2500:return
4460 pf=val(in$):if pf<0 or pf>20 then pf=ef(cu)
4465 l$(ex(cu))=pa$+", "+mid$(str$(pc),2)+", "+mid$(str$(pf),2)
4470 gosub 1200:gosub 2500:gosub 1500:return
4500 if ne=0 then return
4510 m$="delete entry "+mid$(str$(cu+1),2)+"? (y/n)":gosub 7200
4515 if yn=0 then m$="":gosub 7000:return
4520 x=ex(cu):if x<nl-1 then for i=x to nl-2:l$(i)=l$(i+1):next
4530 nl=nl-1:gosub 1200:if cu>=ne then cu=ne-1
4540 if cu<0 then cu=0
4550 gosub 2950:gosub 2550:gosub 1500:return
4600 if cu>=ne-1 then return
4610 a=ex(cu):b=ex(cu+1):t$=l$(a):l$(a)=l$(b):l$(b)=t$:cu=cu+1
4620 gosub 1200:gosub 2950:gosub 2550:gosub 1500:return
4650 if cu<=0 then return
4660 a=ex(cu):b=ex(cu-1):t$=l$(a):l$(a)=l$(b):l$(b)=t$:cu=cu-1
4670 gosub 1200:gosub 2950:gosub 2550:gosub 1500:return
4700 u=1-un:x=d0:if u=1 then x=d1
4710 if x=0 then m$="no drive on that unit":gosub 7000:return
4720 un=u:gosub 880:gosub 1000:gosub 1200:cu=0:tp=0:gosub 2550:return
4800 gosub 3000:gosub 880:gosub 1000:gosub 1200
4810 if cu>=ne then cu=ne-1
4820 if cu<0 then cu=0
4830 gosub 2950:gosub 2550:m$="refreshed":gosub 7000:return
5000 bp$="/":bq=0:bw=0:br=0
5010 gosub 5200
5020 gosub 5400
5030 get k$:if k$="" then 5030
5040 k=asc(k$):if k=95 then return
5050 if k=145 and bq>0 then bq=bq-1:goto 5100
5060 if k=17 and bq<bk-1 then bq=bq+1:goto 5100
5070 if k<>13 or bk=0 then 5030
5080 nn$=bn$(bq):ty$=bt$(bq)
5085 if nn$=".." then 5150
5090 jn$=bp$+"/"+nn$:if bp$="/" then jn$="/"+nn$
5092 if ty$="dir" then bp$=jn$:bq=0:bw=0:goto 5010
5095 bs$=jn$:br=1:return
5100 if bq<bw then bw=bq
5110 if bq>=bw+vr then bw=bq-vr+1
5120 gosub 5400:goto 5030
5150 i=len(bp$)
5155 if mid$(bp$,i,1)<>"/" then i=i-1:goto 5155
5160 bp$=left$(bp$,i-1):if bp$="" then bp$="/"
5170 bq=0:bw=0:goto 5010
5200 bk=0:q$=bp$:if right$(q$,1)<>"/" then q$=q$+"/"
5205 if bp$<>"/" then bn$(0)="..":bt$(0)="dir":bk=1
5210 open1,dv,0,"$"+q$:gosub 800
5215 if en<>0 then close1:m$="cannot list: "+er$:return
5220 get#1,a$:get#1,a$
5240 get#1,a$:get#1,b$
5245 if asc(a$+chr$(0))=0 and asc(b$+chr$(0))=0 then 5345
5250 get#1,a$:get#1,a$
5260 tx$=""
5265 get#1,a$:s=st
5270 if a$="" then 5300
5275 if len(tx$)<60 then tx$=tx$+a$
5280 if (s and 64)=0 then 5265
5300 q1=0:q2=0:j=1
5302 if j>len(tx$) then 5320
5304 c=asc(mid$(tx$,j,1)):if c<>34 then 5308
5306 if q1=0 then q1=j:goto 5308
5307 q2=j:goto 5320
5308 j=j+1:goto 5302
5320 if q2=0 then 5340
5322 nn$=mid$(tx$,q1+1,q2-q1-1)
5324 if right$(nn$,1)=" " then nn$=left$(nn$,len(nn$)-1):goto 5324
5326 ty$=mid$(tx$,q2+1)
5328 if left$(ty$,1)=" " then ty$=mid$(ty$,2):goto 5328
5330 ty$=left$(ty$,3)
5332 if ty$<>"dir" and ty$<>"d64" and ty$<>"d71" and ty$<>"d81" then 5340
5334 if bk<100 then bn$(bk)=nn$:bt$(bk)=ty$:bk=bk+1
5340 if (s and 64)=0 then 5240
5345 close1:return
5400 rw=3:cl=0:gosub 7500
5410 print chr$(18);left$("browse: "+bp$+sp$,39);chr$(146);
5420 for r=0 to vr-1:kk=bw+r:rw=4+r:cl=0:gosub 7500
5430 if kk>=bk then print left$(sp$,39);:goto 5460
5440 a$=" ":if kk=bq then a$=">"
5445 print a$;:if kk=bq then print chr$(18);
5450 print left$(bn$(kk)+sp$,17);chr$(146);" ";bt$(kk);left$(sp$,17);
5460 next
5470 rw=21:cl=0:gosub 7500
5480 print left$("return:open/select  "+chr$(95)+":cancel"+sp$,39);
5490 rw=22:cl=0:gosub 7500:print left$(sp$,39);:rw=23:cl=0:gosub 7500:print left$(sp$,39);:return
7000 rw=20:cl=0:gosub 7500:print left$(m$+sp$,39);:return
7100 cn=0:s$=in$
7110 fw=38-len(lb$)
7120 v$=s$+"_":if len(v$)>fw then v$=".."+right$(v$,fw-2)
7130 rw=23:cl=0:gosub 7500:print lb$;left$(v$+sp$,fw);
7140 get k$:if k$="" then 7140
7150 k=asc(k$):if k=13 then in$=s$:return
7160 if k=95 then cn=1:return
7170 if k=20 and len(s$)>0 then s$=left$(s$,len(s$)-1):goto 7120
7180 if k>=32 and k<95 and len(s$)<ml then s$=s$+k$
7190 goto 7120
7200 gosub 7000
7210 get k$:if k$="" then 7210
7220 yn=0:if k$="y" then yn=1
7230 return
7500 poke 781,rw:poke 782,cl:poke 783,0:sys 65520:return
9000 dim l$(120),ex(32),ep$(32),ec(32),ef(32),cc(15),mp$(1),nm$(1),sn$(1),bn$(100),bt$(100)
9010 for i=0 to 15:read cc(i):next
9020 data 144,5,28,159,156,30,31,158,129,149,150,151,152,153,154,155
9030 sp$=" ":for i=1 to 5:sp$=sp$+sp$:next:sp$=sp$+left$(sp$,8)
9040 nm$(0)="sd ":nm$(1)="usb":sn$(0)="sd":sn$(1)="usb":vr=16
9050 d0=peek(56854):d1=peek(56855)
9060 if d0<8 or d0>30 then d0=0
9070 if d1<8 or d1>30 then d1=0
9080 oc=peek(53280) and 15:ob=peek(53281) and 15:ot=peek(646) and 15
9090 if d0<>0 then open15,d0,15
9095 if d1<>0 then open14,d1,15
9097 poke 53280,0:poke 53281,0:poke 646,1
9099 return
9500 print chr$(147);"vdrive is not active."
9510 print "start this program from the teensyrom menu with the vdrive kernal."
9520 end
9600 close15:close14
9610 poke 53280,oc:poke 53281,ob:poke 646,ot
9620 print chr$(147);chr$(154);"bye"
9630 end
