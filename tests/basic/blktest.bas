10 print chr$(147);"block access test"
20 d=8:input"device";d
30 open15,d,15:gosub 900
40 open2,d,2,"#":gosub 900:t$="open buffer channel":x=(en=0):gosub 700
50 t=35:s=0
60 print#15,"b-a:0";t;s:gosub 900:if en=65 then t=et:s=es:goto 60
70 t$="b-a gets a free sector":x=(en=0):gosub 700
80 print#15,"b-a:0";t;s:gosub 900:t$="b-a on used sector (65)":x=(en=65):gosub 700
90 print#15,"b-p:2,0":for i=0 to 255:print#2,chr$(i);:next
100 print#15,"u2:2,0";t;s:gosub 900:t$="u2 write sector":x=(en=0):gosub 700
110 print#15,"u1:2,0,18,0":gosub 900:t$="u1 read directory header":x=(en=0):gosub 700
120 get#2,a$:t$="header starts with link to 18/1":x=(asc(a$+chr$(0))=18):gosub 700
130 print#15,"u1:2,0";t;s:gosub 900:t$="u1 read back":x=(en=0):gosub 700
140 e=0:for i=0 to 255:get#2,a$:if a$="" then a$=chr$(0)
150 if asc(a$)<>i then e=e+1
160 if i=255 then z=st
170 next:t$="256 bytes match, eoi on last":x=(e=0 and (z and 64)=64):gosub 700
180 print#15,"b-p:2,100":get#2,a$:t$="b-p moves pointer":x=(asc(a$+chr$(0))=100):gosub 700
190 print#15,"u1:2,0,99,0":gosub 900:t$="bad track (66)":x=(en=66):gosub 700
200 print#15,"b-f:0";t;s:gosub 900:t$="b-f frees sector":x=(en=0):gosub 700
210 close2:print#15,"u1:2,0,18,0":gosub 900:t$="closed buffer (70)":x=(en=70):gosub 700
220 print:print p;"passed,";f;"failed"
230 close15:end
700 if x then print"pass: ";t$:p=p+1:return
710 print"FAIL: ";t$;" got";en;et;es:f=f+1:return
900 input#15,en,em$,et,es:return
