#!/usr/bin/env bash
# Fallback: the title's own log file over FTP (the console's /download0), printed from the byte offset
# already seen, once a second. PS5=<console address> is required.
ps5=${PS5:?set PS5 to the console address}
url=ftp://$ps5:2121/mnt/sandbox/PPSA99993_000/download0/ps5-opengl.log
seen=0
while sleep 1; do
  new=$(curl -s --max-time 5 -r "$seen-" "$url" | tee /dev/stdout | wc -c)
  seen=$((seen + new))
done
