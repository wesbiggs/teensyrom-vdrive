10 print chr$(147);"rel file test"
20 d=8:input"device";d
30 open15,d,15:print#15,"s:rt*":gosub 900
40 open2,d,2,"rtrel,l,"+chr$(20):gosub 900:t$="create rel file (len 20)":x=(en=0):gosub 700
50 for r=1 to 30:print#15,"p"chr$(98)chr$(r)chr$(0)chr$(1):print#2,"rec"+str$(r):next
60 gosub 900:t$="write 30 records":x=(en=0 or en=50):gosub 700
70 e=0:for r=30 to 1 step -1:print#15,"p"chr$(98)chr$(r)chr$(0)chr$(1):input#2,a$:if a$<>"rec"+str$(r) then e=e+1
80 next:t$="read 30 records back, reversed":x=(e=0):gosub 700
90 print#15,"p"chr$(98)chr$(5)chr$(0)chr$(5):input#2,a$:t$="position inside a record":x=(a$="5"):gosub 700
100 print#15,"p"chr$(98)chr$(100)chr$(0)chr$(1):gosub 900:t$="record past the end (50)":x=(en=50):gosub 700
110 print#2,"far":print#15,"p"chr$(98)chr$(100)chr$(0)chr$(1):input#2,a$:t$="a write there grows the file":x=(a$="far"):gosub 700
120 print#15,"p"chr$(98)chr$(60)chr$(0)chr$(1):get#2,a$:t$="empty record reads chr$(255)":x=(asc(a$+chr$(0))=255):gosub 700
130 print#15,"p"chr$(98)chr$(1)chr$(0)chr$(1):print#2,"123456789012345678901234":gosub 900:t$="overflow in record (51)":x=(en=51):gosub 700
140 close2:open2,d,2,"rtrel":gosub 900:t$="reopen without a length":x=(en=0):gosub 700
150 print#15,"p"chr$(98)chr$(30)chr$(0)chr$(1):input#2,a$:t$="records kept after close":x=(a$="rec 30"):gosub 700
160 close2:open2,d,2,"rtrel,l,"+chr$(21):gosub 900:t$="other record length (50)":x=(en=50):gosub 700
170 close2:print#15,"c:rtcopy=rtrel":gosub 900:t$="copy a rel file":x=(en=0):gosub 700
180 open2,d,2,"rtcopy":print#15,"p"chr$(98)chr$(100)chr$(0)chr$(1):input#2,a$:t$="the copy has the records":x=(a$="far"):gosub 700
190 close2:print#15,"s:rt*":gosub 900:t$="scratch both":x=(en=1 and et=2):gosub 700
220 print:print p;"passed,";f;"failed"
230 close15:end
700 if x then print"pass: ";t$:p=p+1:return
710 print"FAIL: ";t$;" got";en;et;es:f=f+1:return
900 input#15,en,em$,et,es:return
