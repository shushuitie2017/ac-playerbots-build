#!/usr/bin/env python3
"""把吵架导演源文件注入 mod-ollama-chat 并在 main.cpp 注册。"""
import shutil, sys, pathlib, re

core = sys.argv[1] if len(sys.argv) > 1 else "ac"
repo = pathlib.Path(__file__).resolve().parent.parent          # ci-build 根
src_dir = pathlib.Path(f"{core}/modules/mod-ollama-chat/src")
patch_dir = repo / "module-patches"

for f in ("mod-ollama-chat_drama.h", "mod-ollama-chat_drama.cpp"):
    shutil.copy(patch_dir / f, src_dir / f)
    print(f"copied {f}")

main = src_dir / "mod-ollama-chat_main.cpp"
s = main.read_text(encoding="utf-8")

if "mod-ollama-chat_drama.h" not in s:
    s = s.replace('#include "mod-ollama-chat_rag.h"',
                  '#include "mod-ollama-chat_rag.h"\n#include "mod-ollama-chat_drama.h"')

if "new OllamaDramaDirector();" not in s:
    # 在注册 OllamaBotRandomChatter 之后加一行
    s = s.replace("new OllamaBotRandomChatter();",
                  "new OllamaBotRandomChatter();\n    new OllamaDramaDirector();")

main.write_text(s, encoding="utf-8")
print("registered OllamaDramaDirector in main.cpp")
assert "OllamaDramaDirector" in main.read_text(encoding="utf-8"), "注册失败"
