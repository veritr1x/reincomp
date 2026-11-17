# Report functions that reload an %rsp slot no instruction in the function writes.
/^[A-Za-z_][A-Za-z0-9_.$]*:/ && !/^\.L/ { fn=$1; sub(/:$/,"",fn); delete rel; delete wr; next }
/# [0-9]+-byte Reload/ { if (match($0,/[0-9]+\(%rsp\)/)) rel[substr($0,RSTART,RLENGTH)]=1 }
/, -?[0-9]+\(%rsp\)/ { if (match($0,/, -?[0-9]+\(%rsp\)/)) wr[substr($0,RSTART+2,RLENGTH-2)]=1 }
/\.seh_endproc/ { n=0; s=""; for (k in rel) if (!(k in wr)) { n++; s=s" "k } if (n) print FILENAME, fn, n, s }
