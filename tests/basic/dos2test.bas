10 print chr$(147);"dos test 2: copy, append, lock, wp, v"
20 d=8:input"device";d
30 open15,d,15:print#15,"s:d2*":gosub 900
40 open2,d,2,"d2a,s,w":print#2,"one":close2
50 open2,d,2,"d2b,s,w":print#2,"two":close2
60 print#15,"c:d2c=d2a,d2b":gosub 900:t$="copy joins two files":x=(en=0):gosub 700
70 open2,d,2,"d2c,s,r":input#2,a$,b$:close2:t$="joined data":x=(a$="one" and b$="two"):gosub 700
80 print#15,"c:d2c=d2a":gosub 900:t$="copy onto existing (63)":x=(en=63):gosub 700
90 print#15,"c:d2x=d2none":gosub 900:t$="copy missing source (62)":x=(en=62):gosub 700
100 open2,d,2,"d2a,s,a":gosub 900:t$="open for append":x=(en=0):gosub 700
110 print#2,"three":close2:open2,d,2,"d2a,s,r":input#2,a$,b$:close2:t$="appended data":x=(a$="one" and b$="three"):gosub 700
120 print#15,"l:d2b":gosub 900:print#15,"s:d2b":gosub 900:t$="locked file not scratched":x=(en=1 and et=0):gosub 700
130 print#15,"l:d2b":gosub 900:print#15,"s:d2b":gosub 900:t$="unlocked file scratched":x=(en=1 and et=1):gosub 700
140 print#15,"w-1":gosub 900:open2,d,2,"d2w,s,w":gosub 900:close2:t$="write protect (26)":x=(en=26):gosub 700
150 print#15,"w-0":gosub 900:t$="write protect off":x=(en=0):gosub 700
160 print#15,"v":gosub 900:t$="validate":x=(en=0):gosub 700
170 print#15,"s:d2*":gosub 900:t$="clean up":x=(en=1 and et=2):gosub 700
220 print:print p;"passed,";f;"failed"
230 close15:end
700 if x then print"pass: ";t$:p=p+1:return
710 print"FAIL: ";t$;" got";en;et;es:f=f+1:return
900 input#15,en,em$,et,es:return
