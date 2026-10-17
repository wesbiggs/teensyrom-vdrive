10 if c=0 then c=1:s=0:n=0:e=0
20 if s=1 then 100
30 for i=0 to 5999 step 8:poke 8192+i,0:next
40 s=1:load"big",8,1
100 s=0:n=n+1:x=0
110 for i=0 to 5999 step 8:x=x+peek(8192+i):next
120 if x<>@XX@ then e=e+1:print"mismatch";x
130 print"pass";n;"errors";e
140 get k$:if k$<>"" then end
150 goto 30
