#!/usr/bin/env python3
"""放开奥格瑞玛(zone 1637)内的决斗限制，让机器人能在主城自主互相决斗。

WoW 默认主城无 AREA_FLAG_ALLOW_DUELS 标志 → 三处区域检查会拒绝城内决斗：
  1. 核心   SpellEffects.cpp  Spell::EffectDuel   (施法者 caster + 目标 target 双检查)
  2. mod-playerbots RpgTriggers.cpp  RpgDuelTrigger::IsActive  (目标 player 区域)
  3. mod-playerbots RpgSubActions.cpp RpgDuelAction::isUseful   (自身 bot 区域)

本补丁给每处的区域标志检查追加 `&& XXX->GetZoneId() != 1637`：只要在奥格瑞玛就放行，
其他地方维持原规则(野外可决斗/别的主城仍禁)。用户已拍板：先只开奥格。
(奥格 zone=1637，且不在 playerbots 的 PvpProhibitedZoneIds 默认列表里，无额外拦截。)

配套(非本脚本)：conf 加 AiPlayerbot.RandomBotNonCombatStrategies = "+start duel" 让机器人挂决斗策略。
幂等：已打过则跳过。锚点唯一性校验，失效则 CI 响亮失败。
"""
import sys, pathlib

ORG = "1637"  # 奥格瑞玛 zone id
MARK = "GetZoneId() != 1637"

# (相对 ac 根的文件路径, [(原锚点, 替换), ...])
PATCHES = [
    ("src/server/game/Spells/SpellEffects.cpp", [
        ("if (casterAreaEntry && !(casterAreaEntry->flags & AREA_FLAG_ALLOW_DUELS))",
         f"if (casterAreaEntry && !(casterAreaEntry->flags & AREA_FLAG_ALLOW_DUELS) && caster->GetZoneId() != {ORG})"),
        ("if (targetAreaEntry && !(targetAreaEntry->flags & AREA_FLAG_ALLOW_DUELS))",
         f"if (targetAreaEntry && !(targetAreaEntry->flags & AREA_FLAG_ALLOW_DUELS) && target->GetZoneId() != {ORG})"),
    ]),
    ("modules/mod-playerbots/src/Ai/World/Rpg/Action/RpgSubActions.cpp", [
        ("if (casterAreaEntry && !(casterAreaEntry->flags & AREA_FLAG_ALLOW_DUELS))",
         f"if (casterAreaEntry && !(casterAreaEntry->flags & AREA_FLAG_ALLOW_DUELS) && bot->GetZoneId() != {ORG})"),
    ]),
    ("modules/mod-playerbots/src/Ai/Base/Trigger/RpgTriggers.cpp", [
        ("if (targetAreaEntry && !(targetAreaEntry->flags & AREA_FLAG_ALLOW_DUELS))",
         f"if (targetAreaEntry && !(targetAreaEntry->flags & AREA_FLAG_ALLOW_DUELS) && player->GetZoneId() != {ORG})"),
    ]),
]


def main():
    ac = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "ac")
    for rel, subs in PATCHES:
        f = ac / rel
        src = f.read_text(encoding="utf-8")
        if MARK in src:
            print(f"[orgrimmar-duels] 已打过，跳过：{rel}")
            continue
        for anchor, repl in subs:
            n = src.count(anchor)
            if n != 1:
                print(f"[orgrimmar-duels] 错误：锚点在 {rel} 出现 {n} 次(需唯一)：{anchor}", file=sys.stderr)
                sys.exit(1)
            src = src.replace(anchor, repl, 1)
        f.write_text(src, encoding="utf-8")
        print(f"[orgrimmar-duels] 已放开奥格决斗：{rel}")


if __name__ == "__main__":
    main()
