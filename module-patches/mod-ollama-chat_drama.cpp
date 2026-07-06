#include "mod-ollama-chat_drama.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_api.h"
#include "Log.h"
#include "Config.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ChannelMgr.h"
#include "Channel.h"
#include "SharedDefines.h"
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <random>
#include <algorithm>

OllamaDramaDirector::OllamaDramaDirector() : WorldScript("OllamaDramaDirector") {}

namespace
{
    // 剧情状态（主线程访问，g_dramaMutex 保护）
    std::mutex g_dramaMutex;
    std::string g_topic;
    std::vector<std::string> g_transcript;
    std::vector<uint64_t> g_participants;       // 参与吵架的机器人 GUID
    std::vector<std::string> g_participantNames;
    int g_turn = 0;
    bool g_sceneActive = false;

    // 异步 LLM 产出的待发行（在主线程发送，避免在工作线程碰频道/玩家对象）
    struct PendingLine { uint64_t botGuid; std::string name; std::string text; };
    std::mutex g_pendingMutex;
    std::vector<PendingLine> g_pending;
    bool g_queryInFlight = false;

    const std::vector<std::string> DRAMA_TOPICS = {
        "一个猎人在副本里抢了盗贼的敏捷皮甲装备，盗贼在综合频道开骂，两人对线。",
        "队长把一件板甲装分给了法师(明显分配错误)，战士气炸了在频道理论。",
        "打副本时有个队友全程挂机划水，最后还伸手要装备，大家在频道声讨他。",
        "roll点时有人偷点了不属于自己职业的装备，被当场抓包，在频道对喷。",
        "坦克拉不住仇恨害得团灭，坦克和奶妈互相甩锅一路吵到综合频道。",
        "有人收G(买金币)被骗子跑路了，在综合频道骂骗子，围观的也掺和进来。",
        "打世界BOSS时被别的团抢了先手，两拨人在综合频道互相嘴炮。",
        "一个法师连续几天占着团队副本CD却从不带人，被公会里的人在频道点名。"
    };

    std::string PickTopic()
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> d(0, DRAMA_TOPICS.size() - 1);
        return DRAMA_TOPICS[d(gen)];
    }

    // 主线程：把一句台词发到「有真人玩家在场」的综合频道（General，channelId==1）
    void PostLineToGeneral(PendingLine const& pl, uint32 dramaZone)
    {
        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(pl.botGuid));
        if (!bot || !bot->IsInWorld())
            return;

        // 收集该区域内的真人玩家 GUID（吵架的观众）
        std::vector<ObjectGuid> realGuids;
        for (auto const& itr : ObjectAccessor::GetPlayers())
        {
            Player* p = itr.second;
            if (!p || !p->IsInWorld())
                continue;
            if (PlayerbotsMgr::instance().GetPlayerbotAI(p))
                continue;  // 跳过机器人
            if (p->GetZoneId() != dramaZone)
                continue;
            realGuids.push_back(p->GetGUID());
        }
        if (realGuids.empty())
            return;  // 没真人观众就不发，省算力

        ChannelMgr* cMgr = ChannelMgr::forTeam(bot->GetTeamId());
        if (!cMgr)
            return;

        for (auto const& kv : cMgr->GetChannels())
        {
            Channel* ch = kv.second;
            if (!ch)
                continue;
            if (ch->GetChannelId() != 1)   // 1 = General(综合)
                continue;

            // 该综合频道里有没有我们的真人观众？
            bool hasAudience = false;
            for (auto const& g : realGuids)
            {
                if (ch->DramaIsOn(g))
                {
                    hasAudience = true;
                    break;
                }
            }
            if (!hasAudience)
                continue;

            // 机器人必须先加入该频道才能发言（Channel::Say 要求 IsOn）
            if (!ch->DramaIsOn(bot->GetGUID()))
                ch->JoinChannel(bot, "");

            ch->Say(bot->GetGUID(), pl.text, LANG_UNIVERSAL);

            if (g_DebugEnabled)
                LOG_INFO("server.loading", "[Ollama Drama] {} -> 综合({}): {}", pl.name, ch->GetNumPlayers(), pl.text);
        }
    }
}

void OllamaDramaDirector::OnUpdate(uint32 diff)
{
    if (!g_Enable)
        return;
    if (!sConfigMgr->GetOption<bool>("OllamaChat.DramaEnable", true))
        return;

    uint32 dramaZone = sConfigMgr->GetOption<uint32>("OllamaChat.DramaZone", 1637);  // 奥格瑞玛

    // 1) 先在主线程把上一轮 LLM 产出的台词发出去
    {
        std::vector<PendingLine> toPost;
        {
            std::lock_guard<std::mutex> lk(g_pendingMutex);
            toPost.swap(g_pending);
        }
        for (auto const& pl : toPost)
            PostLineToGeneral(pl, dramaZone);
    }

    // 2) 计时器
    static uint32_t timer = 0;
    if (timer > diff)
    {
        timer -= diff;
        return;
    }

    uint32 intervalSec    = sConfigMgr->GetOption<uint32>("OllamaChat.DramaIntervalSeconds", 25);
    uint32 participantMax = sConfigMgr->GetOption<uint32>("OllamaChat.DramaParticipants", 3);
    uint32 turnsPerScene  = sConfigMgr->GetOption<uint32>("OllamaChat.DramaTurnsPerScene", 8);
    if (intervalSec < 5) intervalSec = 5;
    timer = intervalSec * 1000;

    // 上一句还在生成就不叠加新请求
    {
        std::lock_guard<std::mutex> lk(g_pendingMutex);
        if (g_queryInFlight)
            return;
    }

    std::lock_guard<std::mutex> lock(g_dramaMutex);

    // 需要开新的一场吵架
    if (!g_sceneActive || g_turn >= (int)turnsPerScene)
    {
        std::vector<Player*> zoneBots;
        for (auto const& itr : ObjectAccessor::GetPlayers())
        {
            Player* p = itr.second;
            if (!p || !p->IsInWorld())
                continue;
            if (!PlayerbotsMgr::instance().GetPlayerbotAI(p))
                continue;
            if (p->GetZoneId() != dramaZone)
                continue;
            zoneBots.push_back(p);
        }

        if (zoneBots.size() < 2)
        {
            g_sceneActive = false;
            return;
        }

        std::shuffle(zoneBots.begin(), zoneBots.end(), std::mt19937(std::random_device{}()));

        uint32 n = participantMax;
        if (n > zoneBots.size())
            n = (uint32)zoneBots.size();
        if (n < 2)
            n = 2;

        g_participants.clear();
        g_participantNames.clear();
        for (uint32 i = 0; i < n; ++i)
        {
            g_participants.push_back(zoneBots[i]->GetGUID().GetRawValue());
            g_participantNames.push_back(zoneBots[i]->GetName());
        }
        g_topic = PickTopic();
        g_transcript.clear();
        g_turn = 0;
        g_sceneActive = true;
    }

    if (g_participants.empty())
    {
        g_sceneActive = false;
        return;
    }

    int idx = g_turn % (int)g_participants.size();
    uint64_t speakerGuid = g_participants[idx];
    std::string speakerName = g_participantNames[idx];

    std::string transcriptStr;
    for (auto const& line : g_transcript)
        transcriptStr += line + "\n";

    std::string prompt =
        "你在玩魔兽世界(巫妖王之怒3.3.5)，奥格瑞玛的综合频道正在吵架。\n"
        "吵架背景：" + g_topic + "\n"
        "目前频道里已有的对话：\n" +
        (transcriptStr.empty() ? "(还没人开口，由你先把这事挑起来)\n" : transcriptStr) +
        "你是玩家「" + speakerName + "」，玩了20年的骨灰级老玩家，满口魔兽黑话"
        "(T/奶/DPS/开荒/毕业/搬砖/收G/挂机/甩锅/秒/仇恨/CD/团本)，现在正在气头上。\n"
        "接着上面的对话继续吵，用简体中文说一句狠话(不超过20个字)，要有火药味、像真人对喷。"
        "只输出这一句话，别加引号、别加名字前缀、别当旁白、别解释你是谁。";

    g_turn++;

    {
        std::lock_guard<std::mutex> lk(g_pendingMutex);
        g_queryInFlight = true;
    }

    // 异步查询 LLM（只做网络请求，不碰任何游戏对象）
    std::thread([speakerGuid, speakerName, prompt]()
    {
        std::string response;
        try
        {
            response = QueryOllamaAPI(prompt);
        }
        catch (...)
        {
            response.clear();
        }

        // 去掉首尾引号/换行/空格
        while (!response.empty() && (response.front() == '"' || response.front() == '\n' || response.front() == ' '))
            response.erase(response.begin());
        while (!response.empty() && (response.back() == '"' || response.back() == '\n' || response.back() == ' '))
            response.pop_back();

        if (!response.empty())
        {
            {
                std::lock_guard<std::mutex> lk(g_dramaMutex);
                g_transcript.push_back(speakerName + "：" + response);
            }
            std::lock_guard<std::mutex> lk(g_pendingMutex);
            g_pending.push_back({ speakerGuid, speakerName, response });
        }

        std::lock_guard<std::mutex> lk(g_pendingMutex);
        g_queryInFlight = false;
    }).detach();
}
