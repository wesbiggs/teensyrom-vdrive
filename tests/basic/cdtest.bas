10 print chr$(147);"cd and mount test"
20 d=8:input"device";d
30 open15,d,15
40 print#15,"cd:/":gosub 900:gosub 800
50 t$="cd:/ lists the card root":x=(left$(h$,3)="sd:" or left$(h$,4)="usb:"):gosub 700
60 print#15,"cd:vdtest.d64":gosub 900:t$="mount vdtest.d64":x=(en=0):gosub 700
70 gosub 800:t$="header shows the disk":x=(left$(h$,6)="vdtest"):gosub 700
80 print#15,"cd:/":gosub 900:gosub 800
90 t$="cd:/ unmounts, back at root":x=(left$(h$,3)="sd:" or left$(h$,4)="usb:"):gosub 700
100 print#15,"cd:nowhere.d64":gosub 900:t$="missing image gives 62":x=(en=62):gosub 700
110 gosub 800:t$="still at root after failed mount":x=(left$(h$,3)="sd:" or left$(h$,4)="usb:"):gosub 700
120 print:print p;"passed,";f;"failed"
130 close15:end
700 if x then print"pass: ";t$:p=p+1:return
710 print"FAIL: ";t$;" header=";h$:f=f+1:return
800 open1,d,0,"$":h$=""
810 for i=1 to 6:get#1,a$:next
820 get#1,a$:if a$<>chr$(34) then 820
830 get#1,a$:if a$<>chr$(34) then h$=h$+a$:goto 830
840 close1:return
900 input#15,en,em$,et,es:return
