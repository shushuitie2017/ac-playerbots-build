#ifndef MOD_OLLAMA_CHAT_MEMORY_H
#define MOD_OLLAMA_CHAT_MEMORY_H

#include "ScriptMgr.h"
#include "Player.h"
#include <string>
#include <cstdint>

// 跨天长期记忆 + 关系状态（存 characters 库，按 (bot_guid, player_guid) 归档）
// 读：GenerateBotPrompt 锚点补丁处调 GetMemorySegment，记忆段搭 {chat_history} 便车进提示词
// 写：规则事件（组队/决斗/团灭）零成本实时记 + LLM 低频异步摘要
namespace OllamaChatMemory
{
    // 主线程调用（bot 即将回复该玩家时）：返回记忆段并顺手记账（familiarity+1、last_seen 更新）
    std::string GetMemorySegment(uint64_t botGuid, uint64_t playerGuid);

    // 记一条长期记忆（线程安全，只碰内部结构，可从工作线程调）
    void RecordMemory(uint64_t botGuid, uint64_t playerGuid, const std::string& kind,
                      const std::string& content, float weight = 1.0f);

    // 调整关系（好感度/熟悉度增量，自动升降 tag）
    void AdjustRelationship(uint64_t botGuid, uint64_t playerGuid, int affinityDelta, int famDelta);
}

// 世界脚本：启动建表加载、定时落库、摘要扫描、重逢问候、组队记账
class OllamaMemoryWorldScript : public WorldScript
{
public:
    OllamaMemoryWorldScript();
    void OnStartup() override;
    void OnUpdate(uint32 diff) override;
    void OnShutdown() override;
};

// 玩家事件：决斗胜负、同队团灭（规则事件写入，不花 LLM）
class OllamaMemoryPlayerEvents : public PlayerScript
{
public:
    OllamaMemoryPlayerEvents();
    void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type);
    void OnPlayerJustDied(Player* player);
};

#endif // MOD_OLLAMA_CHAT_MEMORY_H
