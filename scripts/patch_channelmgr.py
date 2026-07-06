#!/usr/bin/env python3
"""给核心 ChannelMgr 加一个公开的频道遍历接口，供吵架导演定位真人所在的综合频道。"""
import sys, pathlib

core = sys.argv[1] if len(sys.argv) > 1 else "ac"
p = pathlib.Path(f"{core}/src/server/game/Chat/Channels/ChannelMgr.h")
s = p.read_text(encoding="utf-8")

anchor = "Channel* GetChannel(std::string const& name, Player* p, bool pkt = true);"
getter = anchor + "\n    ChannelMap& GetChannels() { return channels; }"

if "ChannelMap& GetChannels()" in s:
    print("GetChannels already present")
elif anchor in s:
    s = s.replace(anchor, getter)
    p.write_text(s, encoding="utf-8")
    print("added GetChannels() to ChannelMgr.h")
else:
    print("ANCHOR NOT FOUND — ChannelMgr.h layout changed", file=sys.stderr)
    sys.exit(1)
