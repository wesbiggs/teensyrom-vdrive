10 print chr$(147);"dos command test"
20 d=8:input"device";d
30 open15,d,15:gosub 900
40 for k=1 to 3
50 n$="dt"+chr$(48+k):gosub 800
60 next
70 print#15,"r:dtnew=dt1":gosub 900:t$="rename dt1 to dtnew":x=(en=0):gosub 700
80 n$="dt1":gosub 850:t$="old name gone (62)":x=(en=62):gosub 700
90 n$="dtnew":gosub 850:t$="new name opens":x=(en=0):gosub 700
100 print#15,"r:dt2=dtnew":gosub 900:t$="rename onto existing (63)":x=(en=63):gosub 700
110 print#15,"s:dt2":gosub 900:t$="scratch one file (01,1)":x=(en=1 and et=1):gosub 700
120 n$="dt2":gosub 850:t$="scratched file gone (62)":x=(en=62):gosub 700
130 print#15,"s:dt*":gosub 900:t$="wildcard scratch, 2 files":x=(en=1 and et=2):gosub 700
140 n$="dt9":gosub 800
150 open2,d,2,"dt9,s,w":gosub 900:close2
160 t$="create existing refused (63)":x=(en=63):gosub 700
170 print#15,"s:dt9":gosub 900
180 print#15,"x:foo":gosub 900:t$="unknown command (31)":x=(en=31):gosub 700
190 print:print p;"passed,";f;"failed"
200 close15:end
700 if x then print"pass: ";t$:p=p+1:return
710 print"FAIL: ";t$;" got";en;et:f=f+1:return
800 open2,d,2,n$+",s,w":print#2,"x":close2:gosub 900:return
850 open2,d,2,n$+",s,r":gosub 900:close2:return
900 input#15,en,em$,et,es:return
