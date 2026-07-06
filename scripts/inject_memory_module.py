#!/usr/bin/env python3
"""把跨天长期记忆模块注入 mod-ollama-chat：
1) 拷贝 mod-ollama-chat_memory.{h,cpp} 进 src/（模块 CMake 自动 glob，参与编译）
2) main.cpp 注册 OllamaMemoryWorldScript + OllamaMemoryPlayerEvents
3) handler.cpp 的 GenerateBotPrompt 里把记忆段前置进 {chat_history}（唯一上游锚点补丁）
锚点缺失 → exit 1 让 CI 响亮失败（对齐 patch_channelmgr.py 约定）。
"""
import shutil, sys, pathlib

core = sys.argv[1] if len(sys.argv) > 1 else "ac"
repo = pathlib.Path(__file__).resolve().parent.parent          # ci-build 根
src_dir = pathlib.Path(f"{core}/modules/mod-ollama-chat/src")
patch_dir = repo / "module-patches"

# 1) 拷贝源文件
for f in ("mod-ollama-chat_memory.h", "mod-ollama-chat_memory.cpp"):
    shutil.copy(patch_dir / f, src_dir / f)
    print(f"copied {f}")

# 2) main.cpp 注册
main = src_dir / "mod-ollama-chat_main.cpp"
s = main.read_text(encoding="utf-8")

if "mod-ollama-chat_memory.h" not in s:
    anchor = '#include "mod-ollama-chat_rag.h"'
    if anchor not in s:
        print(f"FATAL: include anchor not found in {main}")
        sys.exit(1)
    s = s.replace(anchor, anchor + '\n#include "mod-ollama-chat_memory.h"')

if "new OllamaMemoryWorldScript();" not in s:
    anchor = "new OllamaBotRandomChatter();"
    if anchor not in s:
        print(f"FATAL: registration anchor not found in {main}")
        sys.exit(1)
    s = s.replace(anchor,
                  anchor + "\n    new OllamaMemoryWorldScript();\n    new OllamaMemoryPlayerEvents();")

main.write_text(s, encoding="utf-8")
assert "OllamaMemoryWorldScript" in main.read_text(encoding="utf-8"), "main.cpp 注册失败"
print("registered memory scripts in main.cpp")

# 3) handler.cpp：记忆段搭 {chat_history} 便车
handler = src_dir / "mod-ollama-chat_handler.cpp"
h = handler.read_text(encoding="utf-8")

if "OllamaChatMemory::GetMemorySegment" not in h:
    inc_anchor = '#include "mod-ollama-chat_sentiment.h"'
    if inc_anchor not in h:
        print(f"FATAL: include anchor not found in {handler}")
        sys.exit(1)
    h = h.replace(inc_anchor, inc_anchor + '\n#include "mod-ollama-chat_memory.h"')

    call_anchor = "std::string chatHistory         = GetBotHistoryPrompt(botGuid, playerGuid, playerMessage);"
    if call_anchor not in h:
        print(f"FATAL: GetBotHistoryPrompt call anchor not found in {handler}")
        sys.exit(1)
    h = h.replace(call_anchor,
                  "std::string chatHistory         = OllamaChatMemory::GetMemorySegment(botGuid, playerGuid) + GetBotHistoryPrompt(botGuid, playerGuid, playerMessage);")

    handler.write_text(h, encoding="utf-8")

assert "OllamaChatMemory::GetMemorySegment" in handler.read_text(encoding="utf-8"), "handler.cpp 补丁失败"
print("patched GenerateBotPrompt to inject memory segment via {chat_history}")
