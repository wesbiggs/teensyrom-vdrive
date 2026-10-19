10 print chr$(147);"raw byte read check"
20 d=8:input"device";d
30 open15,d,15
40 open2,d,2,"rawtst,s,w":for i=1 to 100:print#2,"line,";i;",check,";i*7:next:close2
50 for p=1 to 2
60 print"pass";p:open2,d,2,"rawtst,s,r":e=0:o=0
70 for i=1 to 100
80 x$="line,"+str$(i)+" ,check,"+str$(i*7)+" "+chr$(13)
90 for k=1 to len(x$)
100 get#2,a$:s=st:if a$="" then a$=chr$(0)
110 g=asc(a$):w=asc(mid$(x$,k,1))
120 if g<>w then e=e+1:if e<=30 then print o;"got";g;"exp";w;"st";s
130 o=o+1:next k,i
140 close2:print o;"bytes,";e;"errors"
150 next p
160 print#15,"s:rawtst":close15
