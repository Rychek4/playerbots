# Bridge transport smoke test

Runs the real `BridgeServer` without a game world and drives it with the
Python client from `Azeroth_Narrator`. This folder is not part of the module
build (the CMake glob is not recursive); link the harness by hand from a
configured build tree of the core:

```
cd <build dir>
LINK=$(ninja -t commands src/mangosd/mangosd | tail -1); LIBS="${LINK#*-o src/mangosd/mangosd }"
FLAGS=$(ninja -t commands _deps/playerbots-build/CMakeFiles/playerbots.dir/playerbot/bridge/BridgeServer.cpp.o | tail -1 | tr ' ' '\n' | grep -E '^-I|^-D|^-std|^-Wno' | tr '\n' ' ')
eval g++ -O0 $FLAGS -o /tmp/harness <module>/playerbot/bridge/smoke/main.cpp \
  _deps/playerbots-build/CMakeFiles/playerbots.dir/playerbot/bridge/BridgeServer.cpp.o -pthread $LIBS

/tmp/harness 18890 &
python3 <module>/playerbot/bridge/smoke/smoke.py 18890 <path to Azeroth_Narrator>
```

The harness answers `echo` with its arguments, rejects other commands, sends a
`tick` event every 100 ms, and stops on `quit`. The script checks the hello,
ping, replies, event sequencing, a second client, malformed lines, and the
oversize-line defence.
