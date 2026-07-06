#ifndef MOD_OLLAMA_CHAT_DRAMA_H
#define MOD_OLLAMA_CHAT_DRAMA_H

#include "ScriptMgr.h"

// 吵架导演：定时让奥格瑞玛综合频道里 2-3 个机器人围绕一个剧情话题互相对喷。
class OllamaDramaDirector : public WorldScript
{
public:
    OllamaDramaDirector();
    void OnUpdate(uint32 diff) override;
};

#endif  // MOD_OLLAMA_CHAT_DRAMA_H
