SIP calls to wavs 

Usage:
```
tshark -n -q -r PCAPs/sip-rtp-g729a.pcap -Xlua_script:write-splitted-voip-with-db.lua
tshark -n -q -ieth0 -Xlua_script:write-splitted-voip-with-db.lua
```

write-splitted-voip-with-db.lua refers to ./payload2wav . ./payload2wav converts rtp payload to a wav file.
use
```
make
```
to build ./payload2wav. payload2wav depends on bcg729 lib. 

write-splitted-voip-with-db.lua depends on luasql and postgres table cdr . 
You may just comment driver.postgres, env:connect, con:execute and to function without Postgres.
payload2wav depends on bcg729 lib.  

You can use payload2wav on a payload file (e.g. G711mu data):
```
payload2wav /data/pcaps/12013223\@200.57.7.195_1492336106.8
payload2wav /data/pcaps/12013223\@200.57.7.195_1492336106.8 /data/wavfiles/
```
There is also the INOTIFY variant of the payload2wav here. It converts payload files to WAV in a specified directory on an event IN_CLOSE_WRITE. 
It's just run independently from a payload generator:
```
inotify-payload2wav /data/pcaps1/payload /data/pcaps1/wav
```

tap-rtpsave.c is a tap extension for tshark which working like write-splitted-voip-with-db.lua but faster. Usage:
./tshark -n -q -r sip-rtp-g729a.pcap -z rtp,save
./tshark -n -q -i eth0 -z rtp,save

To build tshark with it, you need to put tap-rtpsave.c in the ui/cli subdirectory of wireshark sources, add it to CMakeLists.txt,ui/cli/Makefile and to ui/cli/tshark-tap-register.c  
(or, if you want to do patching befor ./configure , to CMakeLists.txt,ui/cli/Makefile.am,ui/cli/Makefile.in and to ui/cli/tshark-tap-register.c).  

Enjoy!
