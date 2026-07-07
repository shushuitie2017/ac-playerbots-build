#!/usr/bin/env python3
"""让 mod-ah-bot 把坐骑(class 15 Miscellaneous / subclass 5 Mount)全部纳入出售池。

mod-ah-bot 默认过滤：非商品类物品只有「NPC商贩出售」或「掉落」才进池，且 BoP 绑定被
Bonding 过滤挡掉。坐骑几乎全是商贩坐骑(Vendor)或 BoP 掉落坐骑 → 全被排除。

本补丁在物品加载循环顶部插入一段：只要是坐骑(class 15 / subclass 5，有售价、品质≤6)，
就按品质直接塞进对应 ItemsBin 并 continue，绕过绑定/商贩/掉落/职业/等级等所有过滤。
用户已拍板：全部坐骑(含 BoP，约 320 件)都要能在拍卖行买到。
BoP 坐骑玩家买到即绑定、无法转卖，故不影响防套利。
幂等：已打过则跳过。
"""
import sys, pathlib

MARKER = "AHBOT MOUNT PATCH"

ANCHOR = """    for (ItemTemplateContainer::const_iterator itr = its->begin(); itr != its->end(); ++itr)
    {
"""

INJECT = """    for (ItemTemplateContainer::const_iterator itr = its->begin(); itr != its->end(); ++itr)
    {

        //
        // AHBOT MOUNT PATCH: 坐骑(class 15 Miscellaneous / subclass 5 Mount)绕过
        // 绑定/商贩/掉落/职业/等级等全部过滤，直接按品质纳入出售池。
        //
        if (itr->second.Class == 15 && itr->second.SubClass == 5 &&
            itr->second.SellPrice > 0 && itr->second.Quality <= 6)
        {
            switch (itr->second.Quality)
            {
            case AHB_GREY:   GreyItemsBin.insert(itr->second.ItemId);   break;
            case AHB_WHITE:  WhiteItemsBin.insert(itr->second.ItemId);  break;
            case AHB_GREEN:  GreenItemsBin.insert(itr->second.ItemId);  break;
            case AHB_BLUE:   BlueItemsBin.insert(itr->second.ItemId);   break;
            case AHB_PURPLE: PurpleItemsBin.insert(itr->second.ItemId); break;
            case AHB_ORANGE: OrangeItemsBin.insert(itr->second.ItemId); break;
            case AHB_YELLOW: YellowItemsBin.insert(itr->second.ItemId); break;
            }
            continue;
        }
"""


def main():
    ac = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "ac")
    f = ac / "modules" / "mod-ah-bot" / "src" / "AuctionHouseBotConfig.cpp"
    src = f.read_text(encoding="utf-8")
    if MARKER in src:
        print(f"[patch_ahbot_mounts] 已打过补丁，跳过：{f}")
        return
    if ANCHOR not in src:
        print(f"[patch_ahbot_mounts] 错误：锚点未找到，上游可能已漂移：{f}", file=sys.stderr)
        sys.exit(1)
    if src.count(ANCHOR) != 1:
        print(f"[patch_ahbot_mounts] 错误：锚点出现 {src.count(ANCHOR)} 次，需唯一", file=sys.stderr)
        sys.exit(1)
    f.write_text(src.replace(ANCHOR, INJECT, 1), encoding="utf-8")
    print(f"[patch_ahbot_mounts] 已注入坐骑纳入补丁：{f}")


if __name__ == "__main__":
    main()
