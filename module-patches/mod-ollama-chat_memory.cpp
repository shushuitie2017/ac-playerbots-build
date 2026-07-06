#include "mod-ollama-chat_memory.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat-utilities.h"
#include "Log.h"
#include "Config.h"
#include "Player.h"
#include "Group.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "DatabaseEnv.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
    // ---------- 数据结构（g_memMutex 保护；工作线程只碰这些，不碰游戏对象） ----------
    struct MemoryEntry
    {
        std::string kind;      // fact / event / joke / summary
        std::string content;   // 一条短记忆
        float weight = 1.0f;
        time_t created = 0;
        time_t lastUsed = 0;
    };

    struct Relationship
    {
        int affinity = 0;          // 好感度（可负）
        int familiarity = 0;       // 互动次数=熟悉度
        std::string tag = "陌生人"; // 陌生人/眼熟/队友/朋友/死对头
        time_t firstMet = 0;
        time_t lastSeen = 0;
    };

    struct PairKey
    {
        uint64_t bot;
        uint64_t player;
        bool operator==(PairKey const& o) const { return bot == o.bot && player == o.player; }
    };
    struct PairHash
    {
        size_t operator()(PairKey const& k) const
        {
            return std::hash<uint64_t>()(k.bot) ^ (std::hash<uint64_t>()(k.player) << 1);
        }
    };

    std::mutex g_memMutex;
    std::unordered_map<PairKey, std::vector<MemoryEntry>, PairHash> g_mem;
    std::unordered_map<PairKey, Relationship, PairHash> g_rel;
    bool g_memDirty = false;
    time_t g_lastMemorySave = 0;

    // 摘要扫描状态（主线程访问）
    struct SummaryState
    {
        std::string lastBack;      // 上次看到的最新一条（检测新增）
        int newTurns = 0;          // 上次摘要后累计的新对话轮数（近似）
        time_t lastActivity = 0;
    };
    std::unordered_map<PairKey, SummaryState, PairHash> g_sumState;
    std::atomic<int> g_summariesInFlight{0};
    std::atomic<int> g_greetsInFlight{0};

    // 重逢问候：工作线程产出 → 主线程发送
    struct PendingSay { uint64_t botGuid; std::string text; };
    std::mutex g_pendingMutex;
    std::vector<PendingSay> g_pendingSays;

    // 每日去重（值 = 年*366+年内日序）
    std::unordered_map<PairKey, uint32_t, PairHash> g_lastGreetDay;
    std::unordered_map<PairKey, uint32_t, PairHash> g_lastGroupDay;

    // ---------- 配置 ----------
    bool     CfgEnable()            { return sConfigMgr->GetOption<bool>("OllamaChat.MemoryEnable", true); }
    bool     CfgDebug()             { return sConfigMgr->GetOption<bool>("OllamaChat.MemoryDebug", false) || g_DebugEnabled; }
    uint32_t CfgMaxEntries()        { return sConfigMgr->GetOption<uint32>("OllamaChat.MemoryMaxEntriesPerPair", 25); }
    uint32_t CfgInjectCount()       { return sConfigMgr->GetOption<uint32>("OllamaChat.MemoryInjectCount", 6); }
    uint32_t CfgSummaryMinTurns()   { return sConfigMgr->GetOption<uint32>("OllamaChat.MemorySummaryMinTurns", 5); }
    uint32_t CfgSummaryIdleSec()    { return sConfigMgr->GetOption<uint32>("OllamaChat.MemorySummaryIdleSeconds", 300); }
    uint32_t CfgReunionChance()     { return sConfigMgr->GetOption<uint32>("OllamaChat.MemoryReunionChance", 40); }
    float    CfgReunionDistance()   { return sConfigMgr->GetOption<float>("OllamaChat.MemoryReunionDistance", 30.0f); }
    uint32_t CfgSaveIntervalMin()   { return sConfigMgr->GetOption<uint32>("OllamaChat.MemorySaveIntervalMinutes", 10); }

    // ---------- 小工具 ----------
    bool IsBotPlayer(Player* p)
    {
        return p && PlayerbotsMgr::instance().GetPlayerbotAI(p) != nullptr;
    }

    uint32_t DayKey(time_t t)
    {
        if (t <= 0)
            return 0;
        struct tm lt;
        localtime_r(&t, &lt);
        return (uint32_t)((lt.tm_year + 1900) * 366 + lt.tm_yday);
    }

    // 上次见面的人性化描述（用更新前的 lastSeen）
    std::string AgoText(time_t lastSeen, time_t now)
    {
        if (lastSeen <= 0)
            return "第一次见";
        if (now - lastSeen < 600)
            return "刚刚";
        uint32_t dNow = DayKey(now), dLast = DayKey(lastSeen);
        if (dNow == dLast)
            return "今天早些时候";
        if (dNow == dLast + 1)
            return "昨天";
        uint32_t days = dNow - dLast;
        if (days > 365)
            return "很久以前";
        return SafeFormat("{}天前", days);
    }

    void Retag(Relationship& rel)
    {
        if (rel.affinity <= -6)
            rel.tag = "死对头";
        else if (rel.familiarity >= 30)
            rel.tag = "朋友";
        else if (rel.familiarity >= 12)
            rel.tag = "队友";
        else if (rel.familiarity >= 3)
            rel.tag = "眼熟";
        else
            rel.tag = "陌生人";
    }

    // UTF-8 安全截断（不切碎多字节字符）
    std::string Utf8Truncate(const std::string& s, size_t maxBytes)
    {
        if (s.size() <= maxBytes)
            return s;
        size_t i = maxBytes;
        while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80)
            --i;
        return s.substr(0, i);
    }

    std::string TrimLine(std::string s)
    {
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        // 去掉常见列表前缀与引号
        while (!s.empty() && (s.front() == '-' || s.front() == '*' || s.front() == '"'
               || s.front() == ' ' || std::isdigit(static_cast<unsigned char>(s.front()))
               || s.front() == '.' || s.front() == ')'))
            s.erase(s.begin());
        while (!s.empty() && (s.back() == '"' || s.back() == ' '))
            s.pop_back();
        return s;
    }

    // ---------- 建表 / 加载 / 落库 ----------
    void EnsureTables()
    {
        CharacterDatabase.Execute(
            "CREATE TABLE IF NOT EXISTS mod_ollama_chat_memory ("
            "id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
            "bot_guid BIGINT UNSIGNED NOT NULL,"
            "player_guid BIGINT UNSIGNED NOT NULL,"
            "kind VARCHAR(16) NOT NULL DEFAULT 'fact',"
            "content VARCHAR(255) NOT NULL,"
            "weight FLOAT NOT NULL DEFAULT 1.0,"
            "created_at DATETIME NOT NULL,"
            "last_used DATETIME NOT NULL,"
            "INDEX idx_bp (bot_guid, player_guid)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

        CharacterDatabase.Execute(
            "CREATE TABLE IF NOT EXISTS mod_ollama_chat_relationship ("
            "bot_guid BIGINT UNSIGNED NOT NULL,"
            "player_guid BIGINT UNSIGNED NOT NULL,"
            "affinity INT NOT NULL DEFAULT 0,"
            "familiarity INT NOT NULL DEFAULT 0,"
            "tag VARCHAR(32) NOT NULL DEFAULT '陌生人',"
            "first_met DATETIME NOT NULL,"
            "last_seen DATETIME NOT NULL,"
            "PRIMARY KEY (bot_guid, player_guid)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    }

    void LoadFromDB()
    {
        std::lock_guard<std::mutex> lock(g_memMutex);
        g_mem.clear();
        g_rel.clear();

        if (QueryResult r = CharacterDatabase.Query(
                "SELECT bot_guid, player_guid, kind, content, weight,"
                " UNIX_TIMESTAMP(created_at), UNIX_TIMESTAMP(last_used)"
                " FROM mod_ollama_chat_memory"))
        {
            do
            {
                Field* f = r->Fetch();
                PairKey key{ f[0].Get<uint64_t>(), f[1].Get<uint64_t>() };
                MemoryEntry e;
                e.kind = f[2].Get<std::string>();
                e.content = f[3].Get<std::string>();
                e.weight = f[4].Get<float>();
                e.created = (time_t)f[5].Get<uint64_t>();
                e.lastUsed = (time_t)f[6].Get<uint64_t>();
                g_mem[key].push_back(std::move(e));
            } while (r->NextRow());
        }

        if (QueryResult r = CharacterDatabase.Query(
                "SELECT bot_guid, player_guid, affinity, familiarity, tag,"
                " UNIX_TIMESTAMP(first_met), UNIX_TIMESTAMP(last_seen)"
                " FROM mod_ollama_chat_relationship"))
        {
            do
            {
                Field* f = r->Fetch();
                PairKey key{ f[0].Get<uint64_t>(), f[1].Get<uint64_t>() };
                Relationship rel;
                rel.affinity = f[2].Get<int32>();
                rel.familiarity = f[3].Get<int32>();
                rel.tag = f[4].Get<std::string>();
                rel.firstMet = (time_t)f[5].Get<uint64_t>();
                rel.lastSeen = (time_t)f[6].Get<uint64_t>();
                g_rel[key] = std::move(rel);
            } while (r->NextRow());
        }

        size_t memCount = 0;
        for (auto const& kv : g_mem)
            memCount += kv.second.size();
        LOG_INFO("server.loading", "[Ollama Memory] Loaded {} memories / {} relationships from DB", memCount, g_rel.size());
    }

    void SaveToDB()
    {
        std::lock_guard<std::mutex> lock(g_memMutex);
        if (!g_memDirty)
            return;

        time_t now = time(nullptr);

        // 衰减：7 天没被用到的记忆缓慢降权
        for (auto& kv : g_mem)
            for (auto& e : kv.second)
                if (now - e.lastUsed > 7 * 86400)
                    e.weight *= 0.95f;

        for (auto const& kv : g_rel)
        {
            std::string escTag = kv.second.tag;
            CharacterDatabase.EscapeString(escTag);
            CharacterDatabase.Execute(SafeFormat(
                "REPLACE INTO mod_ollama_chat_relationship"
                " (bot_guid, player_guid, affinity, familiarity, tag, first_met, last_seen)"
                " VALUES ({}, {}, {}, {}, '{}', FROM_UNIXTIME({}), FROM_UNIXTIME({}))",
                kv.first.bot, kv.first.player, kv.second.affinity, kv.second.familiarity,
                escTag, (uint64_t)kv.second.firstMet, (uint64_t)kv.second.lastSeen));
        }

        for (auto const& kv : g_mem)
        {
            CharacterDatabase.Execute(SafeFormat(
                "DELETE FROM mod_ollama_chat_memory WHERE bot_guid = {} AND player_guid = {}",
                kv.first.bot, kv.first.player));
            for (auto const& e : kv.second)
            {
                std::string escKind = e.kind, escContent = e.content;
                CharacterDatabase.EscapeString(escKind);
                CharacterDatabase.EscapeString(escContent);
                CharacterDatabase.Execute(SafeFormat(
                    "INSERT INTO mod_ollama_chat_memory"
                    " (bot_guid, player_guid, kind, content, weight, created_at, last_used)"
                    " VALUES ({}, {}, '{}', '{}', {:.3f}, FROM_UNIXTIME({}), FROM_UNIXTIME({}))",
                    kv.first.bot, kv.first.player, escKind, escContent, e.weight,
                    (uint64_t)e.created, (uint64_t)e.lastUsed));
            }
        }

        g_memDirty = false;
        if (CfgDebug())
            LOG_INFO("server.loading", "[Ollama Memory] Saved memories/relationships to DB");
    }

    // ---------- 摘要 ----------
    // 主线程收集好 transcript 与名字后，工作线程只做 LLM 网络请求 + 写内部结构
    void SpawnSummaryThread(PairKey key, std::string botName, std::string playerName, std::string transcript)
    {
        g_summariesInFlight++;
        std::thread([key, botName, playerName, transcript]()
        {
            std::string prompt = SafeFormat(
                "你在帮游戏《魔兽世界》里的玩家「{}」整理\"关于玩家「{}」的长期记忆\"。\n"
                "读下面这段两人的对话，提炼出最多3条、值得长期记住的短记忆，例如：他的职业/特点、"
                "你们一起做过的事、共同的梗、他帮过你或坑过你、你对他的整体印象。\n"
                "要求：每条不超过20个字，大白话，像日记里随手记的一笔；没有值得记的就只输出\"无\"。\n"
                "只输出这几条，每行一条，不要编号、不要解释。\n\n对话：\n{}",
                botName, playerName, transcript);

            std::string response;
            try { response = QueryOllamaAPI(prompt); }
            catch (...) { response.clear(); }

            int added = 0;
            std::istringstream iss(response);
            std::string line;
            while (added < 3 && std::getline(iss, line))
            {
                line = TrimLine(line);
                if (line.empty() || line == "无" || line == "无。")
                    continue;
                line = Utf8Truncate(line, 200);
                OllamaChatMemory::RecordMemory(key.bot, key.player, "summary", line, 1.0f);
                ++added;
            }
            if (added > 0 && (sConfigMgr->GetOption<bool>("OllamaChat.MemoryDebug", false) || g_DebugEnabled))
                LOG_INFO("server.loading", "[Ollama Memory] Summarized {} memories for pair ({},{})", added, key.bot, key.player);

            g_summariesInFlight--;
        }).detach();
    }

    // 每 30s：轮询模块自带的对话历史，找「聊了几轮且已闲置」的对子做摘要（免 patch handler 写路径）
    void ScanForSummaries()
    {
        if (g_summariesInFlight.load() >= 2)
            return;

        time_t now = time(nullptr);
        struct Job { PairKey key; std::string transcript; };
        std::vector<Job> jobs;

        {
            std::lock_guard<std::mutex> histLock(g_ConversationHistoryMutex);
            std::lock_guard<std::mutex> memLock(g_memMutex);

            for (auto const& botKv : g_BotConversationHistory)
            {
                for (auto const& playerKv : botKv.second)
                {
                    PairKey key{ botKv.first, playerKv.first };
                    if (g_rel.find(key) == g_rel.end())
                        continue;   // 只处理已建立关系的真人对子
                    auto const& deque = playerKv.second;
                    if (deque.empty())
                        continue;

                    SummaryState& st = g_sumState[key];
                    const std::string& back = deque.back().first + "|" + deque.back().second;
                    if (back != st.lastBack)
                    {
                        st.lastBack = back;
                        st.newTurns++;
                        st.lastActivity = now;
                        continue;   // 还在活跃，先不摘要
                    }

                    if (st.newTurns >= (int)CfgSummaryMinTurns()
                        && st.lastActivity > 0
                        && now - st.lastActivity >= (time_t)CfgSummaryIdleSec())
                    {
                        std::string transcript;
                        for (auto const& turn : deque)
                            transcript += "玩家：" + turn.first + "\n我：" + turn.second + "\n";
                        jobs.push_back({ key, std::move(transcript) });
                        st.newTurns = 0;
                        if (jobs.size() >= 2)
                            break;
                    }
                }
                if (jobs.size() >= 2)
                    break;
            }
        }

        for (auto& job : jobs)
        {
            // 主线程取名字
            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(job.key.bot));
            Player* player = ObjectAccessor::FindPlayer(ObjectGuid(job.key.player));
            std::string botName = bot ? bot->GetName() : "我";
            std::string playerName = player ? player->GetName() : "那个玩家";
            SpawnSummaryThread(job.key, botName, playerName, job.transcript);
        }
    }

    // ---------- 重逢问候 + 组队记账 ----------
    void SpawnGreetThread(uint64_t botGuid, std::string botName, std::string playerName,
                          std::string tag, std::string ago, std::string memories)
    {
        g_greetsInFlight++;
        std::thread([botGuid, botName, playerName, tag, ago, memories]()
        {
            std::string prompt = SafeFormat(
                "你在玩《魔兽世界》，你是玩家「{}」。你碰到了{}见过的玩家「{}」（你们的关系：{}）。\n"
                "{}"
                "用简体中文对他说一句自然的重逢打招呼，不超过15个字，"
                "像老玩家碰到熟人那样随意（比如：哟 又见你了 / 上次那本刷完没 / 怎么又是你）。"
                "按你们的关系拿捏态度：朋友就热络、死对头就阴阳、只是眼熟就淡淡的。"
                "只输出那句话，不要引号、不要解释。",
                botName, ago, playerName, tag,
                memories.empty() ? "" : ("你记得关于他的事：\n" + memories));

            std::string response;
            try { response = QueryOllamaAPI(prompt); }
            catch (...) { response.clear(); }

            response = TrimLine(response);
            if (!response.empty())
            {
                response = Utf8Truncate(response, 120);
                std::lock_guard<std::mutex> lk(g_pendingMutex);
                g_pendingSays.push_back({ botGuid, response });
            }
            g_greetsInFlight--;
        }).detach();
    }

    void ScanReunionAndGroup()
    {
        if (!CfgEnable())
            return;

        time_t now = time(nullptr);
        uint32_t today = DayKey(now);

        // 收集真人与机器人
        std::vector<Player*> realPlayers;
        std::vector<Player*> bots;
        for (auto const& itr : ObjectAccessor::GetPlayers())
        {
            Player* p = itr.second;
            if (!p || !p->IsInWorld())
                continue;
            if (IsBotPlayer(p))
                bots.push_back(p);
            else
                realPlayers.push_back(p);
        }
        if (realPlayers.empty())
            return;

        for (Player* player : realPlayers)
        {
            uint64_t playerGuid = player->GetGUID().GetRawValue();

            // 组队记账：真人组里的机器人，每对每天记一次
            if (Group* grp = player->GetGroup())
            {
                for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    if (!member || member == player || !IsBotPlayer(member))
                        continue;
                    PairKey key{ member->GetGUID().GetRawValue(), playerGuid };
                    if (g_lastGroupDay[key] == today)
                        continue;
                    g_lastGroupDay[key] = today;
                    OllamaChatMemory::RecordMemory(key.bot, key.player, "event", "一起组过队", 1.2f);
                    OllamaChatMemory::AdjustRelationship(key.bot, key.player, 1, 3);
                }
            }

            // 重逢问候：附近的老熟人机器人，跨天 + 概率 + 每日一次
            if (g_greetsInFlight.load() >= 1)
                continue;
            for (Player* bot : bots)
            {
                if (bot->GetMapId() != player->GetMapId())
                    continue;
                if (!bot->IsWithinDist(player, CfgReunionDistance(), false))
                    continue;

                PairKey key{ bot->GetGUID().GetRawValue(), playerGuid };
                std::string tag, ago, memories;
                {
                    std::lock_guard<std::mutex> lock(g_memMutex);
                    auto it = g_rel.find(key);
                    if (it == g_rel.end())
                        continue;
                    Relationship& rel = it->second;
                    if (rel.familiarity < 3 || rel.lastSeen <= 0)
                        continue;
                    if (DayKey(rel.lastSeen) >= today)
                        continue;   // 今天已见过
                    if (g_lastGreetDay[key] == today)
                        continue;
                    g_lastGreetDay[key] = today;   // 先占坑（含掷骰失败，避免每 tick 重掷）
                    if ((uint32_t)(rand() % 100) >= CfgReunionChance())
                        continue;

                    ago = AgoText(rel.lastSeen, now);
                    tag = rel.tag;
                    rel.lastSeen = now;   // 它「看见」了这位玩家
                    g_memDirty = true;

                    auto memIt = g_mem.find(key);
                    if (memIt != g_mem.end())
                    {
                        std::vector<MemoryEntry*> sorted;
                        for (auto& e : memIt->second)
                            sorted.push_back(&e);
                        std::sort(sorted.begin(), sorted.end(), [](MemoryEntry* a, MemoryEntry* b)
                        {
                            if (a->weight != b->weight) return a->weight > b->weight;
                            return a->lastUsed > b->lastUsed;
                        });
                        size_t n = std::min(sorted.size(), (size_t)3);
                        for (size_t i = 0; i < n; ++i)
                            memories += "- " + sorted[i]->content + "\n";
                    }
                }

                if (CfgDebug())
                    LOG_INFO("server.loading", "[Ollama Memory] Reunion greet: {} -> {} ({}, {})",
                             bot->GetName(), player->GetName(), tag, ago);
                SpawnGreetThread(key.bot, bot->GetName(), player->GetName(), tag, ago, memories);
                break;   // 每个真人玩家每 tick 最多触发一个问候
            }
        }
    }

    // 主线程发送工作线程产出的问候
    void FlushPendingSays()
    {
        std::vector<PendingSay> toSay;
        {
            std::lock_guard<std::mutex> lk(g_pendingMutex);
            toSay.swap(g_pendingSays);
        }
        for (auto const& ps : toSay)
        {
            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(ps.botGuid));
            if (!bot || !bot->IsInWorld())
                continue;
            PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
            if (!ai)
                continue;
            ai->Say(ps.text);
            if (CfgDebug())
                LOG_INFO("server.loading", "[Ollama Memory] {} says reunion greeting: {}", bot->GetName(), ps.text);
        }
    }
}

// ---------- 对外 API ----------
namespace OllamaChatMemory
{

std::string GetMemorySegment(uint64_t botGuid, uint64_t playerGuid)
{
    if (!g_Enable || !CfgEnable())
        return "";

    // 只跟踪真人玩家（主线程调用，可安全取对象）
    Player* player = ObjectAccessor::FindPlayer(ObjectGuid(playerGuid));
    if (!player || IsBotPlayer(player))
        return "";

    time_t now = time(nullptr);
    PairKey key{ botGuid, playerGuid };

    std::lock_guard<std::mutex> lock(g_memMutex);

    Relationship& rel = g_rel[key];
    if (rel.firstMet == 0)
        rel.firstMet = now;
    std::string ago = AgoText(rel.lastSeen, now);
    rel.familiarity++;
    Retag(rel);
    rel.lastSeen = now;
    g_memDirty = true;

    // 第一次搭话且啥也不记得：不注入，正常对待
    auto memIt = g_mem.find(key);
    bool hasMemories = memIt != g_mem.end() && !memIt->second.empty();
    if (!hasMemories && rel.tag == "陌生人")
        return "";

    std::string seg = SafeFormat(
        "【你对{}的记忆】关系：{}（好感{}），上次见面：{}。\n",
        player->GetName(), rel.tag,
        rel.affinity > 2 ? "不错" : (rel.affinity < -2 ? "很差" : "一般"), ago);

    if (hasMemories)
    {
        std::vector<MemoryEntry*> sorted;
        for (auto& e : memIt->second)
            sorted.push_back(&e);
        std::sort(sorted.begin(), sorted.end(), [](MemoryEntry* a, MemoryEntry* b)
        {
            if (a->weight != b->weight) return a->weight > b->weight;
            return a->lastUsed > b->lastUsed;
        });
        size_t n = std::min(sorted.size(), (size_t)CfgInjectCount());
        seg += "你记得关于他的事：\n";
        for (size_t i = 0; i < n; ++i)
        {
            seg += "- " + sorted[i]->content + "\n";
            sorted[i]->lastUsed = now;
        }
    }
    seg += "（按这些记忆自然地对待他：朋友就热络、死对头就阴阳、眼熟就淡淡的。绝不把记忆一条条念出来。）\n";

    if (CfgDebug())
        LOG_INFO("server.loading", "[Ollama Memory] Inject segment for pair ({},{}): {} chars", botGuid, playerGuid, seg.size());

    return seg;
}

void RecordMemory(uint64_t botGuid, uint64_t playerGuid, const std::string& kind,
                  const std::string& content, float weight)
{
    if (content.empty())
        return;

    time_t now = time(nullptr);
    PairKey key{ botGuid, playerGuid };

    std::lock_guard<std::mutex> lock(g_memMutex);
    auto& entries = g_mem[key];

    // 去重：内容相同或互相包含 → 合并（刷新时间、略加权重）
    for (auto& e : entries)
    {
        if (e.content == content
            || e.content.find(content) != std::string::npos
            || content.find(e.content) != std::string::npos)
        {
            e.lastUsed = now;
            e.weight = std::min(3.0f, e.weight + 0.2f);
            g_memDirty = true;
            return;
        }
    }

    MemoryEntry e;
    e.kind = kind;
    e.content = Utf8Truncate(content, 200);
    e.weight = weight;
    e.created = now;
    e.lastUsed = now;
    entries.push_back(std::move(e));

    // 封顶：删掉权重最低、最久没用的
    while (entries.size() > CfgMaxEntries())
    {
        auto worst = std::min_element(entries.begin(), entries.end(),
            [](MemoryEntry const& a, MemoryEntry const& b)
            {
                if (a.weight != b.weight) return a.weight < b.weight;
                return a.lastUsed < b.lastUsed;
            });
        entries.erase(worst);
    }
    g_memDirty = true;
}

void AdjustRelationship(uint64_t botGuid, uint64_t playerGuid, int affinityDelta, int famDelta)
{
    time_t now = time(nullptr);
    PairKey key{ botGuid, playerGuid };

    std::lock_guard<std::mutex> lock(g_memMutex);
    Relationship& rel = g_rel[key];
    if (rel.firstMet == 0)
        rel.firstMet = now;
    rel.affinity += affinityDelta;
    rel.familiarity += famDelta;
    Retag(rel);
    g_memDirty = true;
}

} // namespace OllamaChatMemory

// ---------- WorldScript ----------
OllamaMemoryWorldScript::OllamaMemoryWorldScript() : WorldScript("OllamaMemoryWorldScript") {}

void OllamaMemoryWorldScript::OnStartup()
{
    if (!CfgEnable())
    {
        LOG_INFO("server.loading", "[Ollama Memory] Disabled by config");
        return;
    }
    EnsureTables();
    LoadFromDB();
    g_lastMemorySave = time(nullptr);
    LOG_INFO("server.loading", "[Ollama Memory] Long-term memory system initialized");
}

void OllamaMemoryWorldScript::OnUpdate(uint32 diff)
{
    if (!g_Enable || !CfgEnable())
        return;

    // 1) 先把工作线程产出的问候发出去（主线程）
    FlushPendingSays();

    // 2) 10s 节拍：重逢问候 + 组队记账
    static uint32_t scanTimer = 0;
    if (scanTimer <= diff)
    {
        scanTimer = 10000;
        ScanReunionAndGroup();
    }
    else
        scanTimer -= diff;

    // 3) 30s 节拍：摘要扫描
    static uint32_t sumTimer = 0;
    if (sumTimer <= diff)
    {
        sumTimer = 30000;
        ScanForSummaries();
    }
    else
        sumTimer -= diff;

    // 4) 定时落库
    time_t now = time(nullptr);
    if (CfgSaveIntervalMin() > 0 && difftime(now, g_lastMemorySave) >= CfgSaveIntervalMin() * 60)
    {
        g_lastMemorySave = now;
        SaveToDB();
    }
}

void OllamaMemoryWorldScript::OnShutdown()
{
    SaveToDB();
}

// ---------- PlayerScript（规则事件） ----------
OllamaMemoryPlayerEvents::OllamaMemoryPlayerEvents() : PlayerScript("OllamaMemoryPlayerEvents") {}

void OllamaMemoryPlayerEvents::OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType /*type*/)
{
    if (!CfgEnable() || !winner || !loser)
        return;

    bool winnerIsBot = IsBotPlayer(winner);
    bool loserIsBot = IsBotPlayer(loser);

    if (winnerIsBot && !loserIsBot)
    {
        // 机器人赢了真人
        OllamaChatMemory::RecordMemory(winner->GetGUID().GetRawValue(), loser->GetGUID().GetRawValue(),
                                       "event", "决斗赢过他", 1.2f);
        OllamaChatMemory::AdjustRelationship(winner->GetGUID().GetRawValue(), loser->GetGUID().GetRawValue(), 1, 1);
    }
    else if (!winnerIsBot && loserIsBot)
    {
        // 真人赢了机器人
        OllamaChatMemory::RecordMemory(loser->GetGUID().GetRawValue(), winner->GetGUID().GetRawValue(),
                                       "event", "决斗被他赢了", 1.2f);
        OllamaChatMemory::AdjustRelationship(loser->GetGUID().GetRawValue(), winner->GetGUID().GetRawValue(), -1, 1);
    }
}

void OllamaMemoryPlayerEvents::OnPlayerJustDied(Player* player)
{
    if (!CfgEnable() || !player || IsBotPlayer(player))
        return;

    Group* grp = player->GetGroup();
    if (!grp)
        return;

    uint64_t playerGuid = player->GetGUID().GetRawValue();
    for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == player || !IsBotPlayer(member))
            continue;
        OllamaChatMemory::RecordMemory(member->GetGUID().GetRawValue(), playerGuid,
                                       "event", "一起灭过团", 1.2f);
    }
}
