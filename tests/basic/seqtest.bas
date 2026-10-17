10 print chr$(147);"sequential file test"
20 d=8:input"device";d
30 open15,d,15
40 gosub 900
50 n$="seqtst"
60 open2,d,2,n$+",s,w"
70 gosub 900:if en<>0 then print"open w failed:";en;em$:goto 400
80 for i=1 to 100:print#2,"line,";i;",check,";i*7:next
90 close2:gosub 900:print"write status:";en;em$
100 open2,d,2,n$+",s,r"
110 gosub 900:if en<>0 then print"open r failed:";en;em$:goto 400
120 e=0
130 for i=1 to 100
140 input#2,a$,b,c$,v
150 if a$<>"line" or b<>i or c$<>"check" or v<>i*7 then e=e+1:print"bad line";i;a$;b;c$;v
160 next
170 close2:gosub 900
180 print"checked 100 lines,";e;"errors"
190 print"final status:";en;em$
200 if e=0 and en=0 then print"seq test pass":goto 220
210 print"seq test fail"
220 print#15,"s:"+n$:gosub 900:print"scratch:";en;em$
400 close15:end
900 input#15,en,em$,et,es:return
