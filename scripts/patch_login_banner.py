#!/usr/bin/env python3
"""构建前补丁：移除登录时的三行署名横幅。
- 核心 MotdMgr.cpp：故意混淆拼接的 "runs on AzerothCore" 尾巴 → 收敛为不追加
- 模块 Playerbots.cpp：登录钩子里 "runs with mod-playerbots" / "configured with N bots" 两条 SendSysMessage

AGPLv3 合规：本补丁脚本随公开仓库 shushuitie2017/ac-playerbots-build 一同发布，
即为对最终用户提供的“修改后源码可公开访问”。核心 GPLv2 不强制保留登录横幅。
"""
import re, sys, pathlib

def patch(path, subs):
    p = pathlib.Path(path)
    if not p.exists():
        print(f"SKIP (missing): {path}"); return
    s = orig = p.read_text(encoding="utf-8")
    for pat, repl, label in subs:
        s, n = re.subn(pat, repl, s, flags=re.S)
        print(f"  [{label}] {n} replacement(s)")
    if s != orig:
        p.write_text(s, encoding="utf-8")
        print(f"PATCHED: {path}")
    else:
        print(f"NO-CHANGE: {path}")

core = sys.argv[1] if len(sys.argv) > 1 else "ac"
patch(f"{core}/src/server/game/Motd/MotdMgr.cpp", [
    (r"motd = /\* fctlsup.*?;", "motd = motd;", "motd-credit-append"),
])
patch(f"{core}/modules/mod-playerbots/src/Script/Playerbots.cpp", [
    (r"ChatHandler\(player->GetSession\(\)\)\.SendSysMessage\(\s*\"[^\"]*This server runs with[^;]*\);", "{}", "runs-with-line"),
    (r"ChatHandler\(player->GetSession\(\)\)\.SendSysMessage\(\s*\"[^\"]*configured with[^;]*\);", "{}", "bots-count-line"),
])
