#!/usr/bin/env python3
"""给核心 ChannelMgr 加一个公开的频道遍历接口，供吵架导演定位真人所在的综合频道。"""
import sys, pathlib

core = sys.argv[1] if len(sys.argv) > 1 else "ac"

# 1) ChannelMgr.h：公开 GetChannels()
p = pathlib.Path(f"{core}/src/server/game/Chat/Channels/ChannelMgr.h")
s = p.read_text(encoding="utf-8")
anchor = "Channel* GetChannel(std::string const& name, Player* p, bool pkt = true);"
getter = anchor + "\n    ChannelMap& GetChannels() { return channels; }"
if "ChannelMap& GetChannels()" in s:
    print("GetChannels already present")
elif anchor in s:
    p.write_text(s.replace(anchor, getter), encoding="utf-8")
    print("added GetChannels() to ChannelMgr.h")
else:
    print("ANCHOR NOT FOUND — ChannelMgr.h layout changed", file=sys.stderr)
    sys.exit(1)

# 2) Channel.h：公开 DramaIsOn()（IsOn 是私有，加个公开包装供吵架模块用）
p2 = pathlib.Path(f"{core}/src/server/game/Chat/Channels/Channel.h")
s2 = p2.read_text(encoding="utf-8")
anchor2 = "[[nodiscard]] uint32 GetNumPlayers() const { return playersStore.size(); }"
wrapper = anchor2 + "\n    [[nodiscard]] bool DramaIsOn(ObjectGuid who) const { return playersStore.find(who) != playersStore.end(); }"
if "DramaIsOn" in s2:
    print("DramaIsOn already present")
elif anchor2 in s2:
    p2.write_text(s2.replace(anchor2, wrapper), encoding="utf-8")
    print("added DramaIsOn() to Channel.h")
else:
    print("ANCHOR2 NOT FOUND — Channel.h layout changed", file=sys.stderr)
    sys.exit(1)
