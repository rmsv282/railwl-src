#!/bin/bash
# Inspect a captured PPM frame (binary P6, header is exactly 16 bytes:
# "P6\n1360 768\n255\n").  No python, no image libraries: raw bytes via od.
#
#   colormap.sh hist  frame.ppm [stride]   top exact colours
#   colormap.sh map   frame.ppm [xstep ystep]  coarse colour map
#   colormap.sh pixel frame.ppm x y        exact RGB at x,y
f=${2:-live.ppm}
case "$1" in
hist)
  stride=${3:-12}
  echo "top exact colours (sampling every ${stride}th px of $f):"
  od -An -tu1 -v -j 16 "$f" | awk -v st="$stride" '
    { for(i=1;i<=NF;i++) v[n++]=$i }
    END{
      for(i=0;i+2<n;i+=3*st){
        k = (v[i]","v[i+1]","v[i+2]);
        c[k]++
      }
      for(k in c) printf "%d %s\n", c[k], k
    }' | sort -rn | head -12 | awk '{printf "  %7d px  rgb(%s)\n", $1, $2}'
  ;;
pixel)
  off=$(( 16 + ($4*1360 + $3)*3 ))
  printf "rgb( "; od -An -tu1 -j "$off" -N 3 "$f" | tr -s ' ' | sed 's/^ //;s/ /, /g'; printf " )\n"
  ;;
map|*)
  xs=${3:-40}; ys=${4:-24}
  echo "legend: R=red G=green B=blue #=amber c=chrome #475156 .=window #2b2a33 o=other ' '=black/clear"
  for y in $(seq 12 "$ys" 767); do
    line=""
    for x in $(seq 12 "$xs" 1359); do
      off=$(( 16 + (y*1360 + x)*3 ))
      read -r r g b <<< "$(od -An -tu1 -j "$off" -N 3 "$f")"
      if   [ "$r" -gt 200 ] && [ "$g" -lt 90 ] && [ "$b" -lt 90 ]; then c=R
      elif [ "$r" -eq 255 ] && [ "$g" -ge 150 ] && [ "$b" -lt 60 ]; then c='#'
      elif [ "$r" -lt 30 ] && [ "$g" -lt 30 ] && [ "$b" -lt 30 ]; then c=' '
      elif [ "$r" -ge 39 ] && [ "$r" -le 47 ] && [ "$g" -ge 38 ] && [ "$g" -le 46 ]; then c=.
      elif [ "$g" -gt 60 ] && [ "$g" -lt 100 ]; then c=c
      elif [ "$r" -eq 20 ] && [ "$g" -eq 20 ]; then c=' '
      else c=o
      fi
      line="$line$c"
    done
    printf "%4d|%s\n" "$y" "$line"
  done
  ;;
esac
