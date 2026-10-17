10 print chr$(147);"renumber drive test"
20 input"current device";d:input"new device (8-30)";n
30 open15,d,15,"u0>"+chr$(n)
40 open14,n,15
50 input#14,en,em$,et,es
60 print"new device";n;"answers:";en;em$
70 close14
80 close15
90 print"old device";d;"no longer answers (real bus)."
100 print"power-cycle and run again with current=";n;"new=";d
110 print"to prove it stuck; new=";d;"puts it back."
