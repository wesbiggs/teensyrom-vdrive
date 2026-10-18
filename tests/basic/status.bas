10 print chr$(147);"status text probe"
20 d=8:input"device";d
30 open15,d,15
40 print#15,"s:nofile":gosub 100
50 print#15,"r:a=zzz":gosub 100
60 print#15,"x:foo":gosub 100
70 open2,d,2,"nofile,s,r":gosub 100:close2
80 print#15,"i":gosub 100
90 close15:end
100 print"[";:c=0
110 get#15,a$:c=c+1:if a$=chr$(13) or c>60 then print"]":return
120 print a$;:goto 110
