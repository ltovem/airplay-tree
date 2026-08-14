#!/usr/bin/env python3
"""
airplay2lib CI autopilot

用途：
  后台轮询 GitHub Actions 仓库 ltovem/airplay-tree 上主分支最新一次 run。
  对每个失败的 job：
    - 下载其步骤日志（仅失败步骤附近 40 行）
    - 用一组"正则 → 补丁函数"的规则尝试自动修复源码/CI yml
    - 用本机 git 提交并 push 到 origin main
    - 等待新一轮 run 生成，继续循环

  成功终止条件：
    - 所有 job in (success, skipped)
    - 或累计轮次 >= MAX_ROUNDS
    - 或同一错误连续出现但规则无法修复（人工介入）

安全：
  - 绝不 commit 任何包含 "ghp_" / "BEGIN RSA" / "BEGIN PRIVATE KEY" 的内容
  - 每次 commit 前 diff --stat 只允许 <50 文件、<100KB 净增
  - 推使用 SSH (git@github.com...) 与本机 SSH agent，不接触令牌
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time
import urllib.request
import urllib.error
from typing import Dict, List, Tuple

GITHUB_OWNER = "ltovem"
GITHUB_REPO  = "airplay-tree"
WORKFLOW_FILE = "ci.yml"
BRANCH       = "main"
MAX_ROUNDS   = 20
POLL_SECONDS = 75                   # GitHub Actions 每次查别太勤；免费额度有限
FAILED_LINES_CONTEXT = 40           # 每个失败步骤前后抓取的行数
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------
# 简单的 GitHub REST wrapper（只调用公开 endpoints + 未登录时 rate limit 60/小时够用）
# ---------------------------------------------------------------------------
GITHUB_API = "https://api.github.com"

def gh_get(path: str) -> dict:
    url = f"{GITHUB_API}{path}"
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json",
                                                "User-Agent": "airplay2lib-autopilot/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        print(f"[gh] HTTP {e.code} for {path}: {e.read().decode('utf-8', 'replace')[:300]}",
              file=sys.stderr, flush=True)
        return {}

def gh_raw(url: str) -> str:
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json",
                                                "User-Agent": "airplay2lib-autopilot/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        print(f"[gh] HTTP {e.code} raw {url}: {e.read().decode('utf-8', 'replace')[:300]}",
              file=sys.stderr, flush=True)
        return ""

# ---------------------------------------------------------------------------
# Run / jobs 查询
# ---------------------------------------------------------------------------
def find_latest_run() -> dict:
    """找分支 main / workflow=ci.yml 的最新一次 run"""
    data = gh_get(f"/repos/{GITHUB_OWNER}/{GITHUB_REPO}/actions/runs"
                  f"?branch={BRANCH}&per_page=5")
    runs = data.get("workflow_runs", [])
    for r in runs:
        if r.get("name", "").lower().startswith("ci build"):
            return r
    return runs[0] if runs else {}

def list_jobs(run_id: int) -> List[dict]:
    jobs = []
    page = 1
    while True:
        d = gh_get(f"/repos/{GITHUB_OWNER}/{GITHUB_REPO}/actions/runs/{run_id}/jobs"
                   f"?per_page=100&page={page}")
        batch = d.get("jobs", [])
        jobs.extend(batch)
        if len(batch) < 100:
            break
        page += 1
    return jobs

# ---------------------------------------------------------------------------
# 日志抓取（只取失败步骤附近）
# ---------------------------------------------------------------------------
def fetch_failed_log_tail(job: dict) -> str:
    logs_url = job.get("logs_url")
    if not logs_url:
        return ""
    raw = gh_raw(logs_url)
    if not raw:
        return ""
    lines = raw.splitlines()
    # 找含有 "Error" / "error" / "FAILED" / "undefined reference" / 等失败关键词的行
    bad = [i for i, ln in enumerate(lines)
           if any(k in ln for k in (
               "error:", "Error ", "ERROR:", "FAILED", "failed:",
               "undefined reference", "No such file",
               "ld: ", "clang: error", "gcc: error", "MSB",
               "collect2: error", "CMake Error", "ninja: build stopped",
               "make[", "*** [", "fatal error:"))]
    if not bad:
        # 没有明显关键词，就取尾部
        return "\n".join(lines[-FAILED_LINES_CONTEXT * 2:])
    hits: List[str] = []
    for idx in sorted(set(bad)):
        lo = max(0, idx - FAILED_LINES_CONTEXT)
        hi = min(len(lines), idx + FAILED_LINES_CONTEXT)
        hits.append(f"\n----- LOG TAIL AROUND line {idx+1} -----")
        hits.extend(lines[lo:hi])
    return "\n".join(hits)

# ---------------------------------------------------------------------------
# Patch rules: list of (regex -> patch(log, match) -> True if applied)
# ---------------------------------------------------------------------------
def replace_file(path_rel: str, old: str, new: str, strict: bool = True) -> bool:
    p = os.path.join(REPO_ROOT, path_rel)
    try:
        with open(p, "r", encoding="utf-8") as f:
            cur = f.read()
    except FileNotFoundError:
        print(f"[patch] file missing: {path_rel}")
        return False
    if strict and old not in cur:
        # 已经被改过就不算成功
        print(f"[patch] target block not found in {path_rel}")
        return False
    cur2 = cur.replace(old, new, 1) if strict else cur.replace(old, new)
    if cur2 == cur:
        return False
    with open(p, "w", encoding="utf-8") as f:
        f.write(cur2)
    print(f"[patch] applied -> {path_rel}")
    return True

def patch_toolchain_ios_system_name(log: str, m: re.Match) -> bool:
    # iOS job 若报错 "CFNetwork 找不到" 或 "为 macOS 构建但链接 iOS sdk"，
    # 典型原因是 Toolchain-iOS.cmake 的 CMAKE_SYSTEM_NAME 不是 iOS。
    # 对于 CMake 3.14+ 推荐改成 iOS，自动触发 CMAKE_OSX_SYSROOT 校验、
    # 禁止查主机路径的 find_library（不会误抓 macOS 动态库）。
    return replace_file(
        "cmake/Toolchain-iOS.cmake",
        'set(CMAKE_SYSTEM_NAME Darwin)',
        'set(CMAKE_SYSTEM_NAME iOS)')

def patch_toolchain_ios_find_root(log: str, m: re.Match) -> bool:
    # 上一条改成 iOS 后，需要加上 CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY，
    # 否则 CMake try_compile 会因 iOS 不能直接链接可执行失败（SDK 未签名）。
    return replace_file(
        "cmake/Toolchain-iOS.cmake",
        'set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n',
        'set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n'
        '# iOS 不允许直接 build/run 宿主可执行（需要签名），跳过 try_compile 的链接阶段。\n'
        'set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\n')

def patch_android_tools_version(log: str, m: re.Match) -> bool:
    # Android job 偶发：setup-ndk 输出 ndk-path 但我们传给的 ANDROID_PLATFORM=android-21
    # 如果失败关键词包含 "Unknown platform" 等，通常是 NDK 太新移除了 android-21。
    # 放宽到 android-24 是个稳妥中位数。
    return replace_file(
        ".github/workflows/ci.yml",
        "- { abi: 'armeabi-v7a', api: 21 }\n"
        "          - { abi: 'arm64-v8a',   api: 21 }\n"
        "          - { abi: 'x86_64',      api: 21 }",
        "- { abi: 'armeabi-v7a', api: 24 }\n"
        "          - { abi: 'arm64-v8a',   api: 24 }\n"
        "          - { abi: 'x86_64',      api: 24 }")

def patch_ndk_action_version(log: str, m: re.Match) -> bool:
    # setup-ndk@v1 找不到指定 r26d：回退到 r25c，兼容性更广
    return replace_file(
        ".github/workflows/ci.yml",
        "ndk-version: r26d",
        "ndk-version: r25c")

def patch_missing_symbol(log: str, m: re.Match) -> bool:
    # Linux/GCC/Android 出现 undefined reference to `airplay2::X::Y(...)'，
    # 90% 情况是 CMakeLists.txt AIRPLAY2_SOURCES 里漏加了 .cpp。
    # 我们尝试从报错里抽出方法所在文件名：例如 "alac_decoder.cpp"
    symbol = m.group(1)
    # 最朴素的映射：搜关键词
    keywords = ["alac_decoder", "audio_buffer", "http_parser", "http_server",
                "rtsp_server", "rtp_receiver", "mdns_publisher", "mdns_browser",
                "platform_socket", "platform_thread", "platform_time",
                "airplay_server_impl", "airplay_session_impl", "airplay_pairing",
                "airplay2.cpp"]
    with open(os.path.join(REPO_ROOT, "CMakeLists.txt"), "r", encoding="utf-8") as f:
        cmake = f.read()
    for kw in keywords:
        file_rel = f"src/{kw}" if not kw.endswith(".cpp") else f"src/{kw}"
        if not kw.endswith(".cpp"):
            file_rel += ".cpp"
        base = os.path.basename(file_rel)
        if base not in cmake and (kw.replace("_", "") in symbol.lower()
                                   or kw.split("_")[0] in symbol.lower()):
            # 插入到 set(AIRPLAY2_SOURCES 段最后
            anchor = "    src/core/airplay_pairing.cpp\n)\n"
            addition = f"    {file_rel}\n"
            return replace_file("CMakeLists.txt", anchor, addition + anchor, strict=True)
    return False

# 规则表（按优先级）
RULES: List[Tuple[re.Pattern, callable]] = [
    # CMAKE_SYSTEM_NAME 相关 iOS 链接/配置错误
    (re.compile(r"(CFNetwork|CoreFoundation).*not found.*iOS|"
                r"building for macOS, but linking in.*tbd.*built for iOS|"),
     patch_toolchain_ios_system_name),
    (re.compile(r"CMake Error: CMAKE_(?:C|CXX)_COMPILER not set after EnableLanguage|try_compile.*iOS|"),
     patch_toolchain_ios_find_root),
    # Android
    (re.compile(r"Unknown platform name android-21|platforms/android-21 is not available|"),
     patch_android_tools_version),
    (re.compile(r"setup-ndk.*r26d.*not found|NDK.*r26d.*404|"),
     patch_ndk_action_version),
    # 链接缺符号
    (re.compile(r"undefined reference to `([^']+)'"),
     patch_missing_symbol),
    (re.compile(r"error LNK2019: unresolved external symbol (\S+)"),
     patch_missing_symbol),
]

def try_autopatch(job: dict, log: str) -> bool:
    for pat, fn in RULES:
        m = pat.search(log)
        if m:
            try:
                if fn(log, m):
                    print(f"[autopatch] applied rule {fn.__name__} for job {job['name']}")
                    return True
            except Exception as e:
                print(f"[autopatch] rule {fn.__name__} threw: {e}", file=sys.stderr)
    return False

# ---------------------------------------------------------------------------
# Git commit + push（安全检查内建）
# ---------------------------------------------------------------------------
def git(*args, check: bool = True) -> str:
    res = subprocess.run(["git", "-C", REPO_ROOT, *args],
                         capture_output=True, text=True)
    if check and res.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed:\n{res.stderr}")
    return (res.stdout or "") + (res.stderr or "")

def commit_and_push_if_dirty(tag: str) -> bool:
    # 不允许提交 secrets
    diffname = git("diff", "--cached", "--name-only")
    work   = git("diff", "--name-only")
    untracked = git("ls-files", "--others", "--exclude-standard")
    changed_files = [ln for ln in (diffname + work + untracked).splitlines() if ln.strip()]
    if not changed_files:
        print("[git] nothing changed")
        return False
    # 扫内容
    git("add", "-A")
    diffcontent = git("diff", "--cached")
    for token in ("ghp_", "BEGIN RSA PRIVATE KEY", "BEGIN PRIVATE KEY",
                  "github_pat_", "-----BEGIN EC PRIVATE KEY-----"):
        if token in diffcontent:
            raise RuntimeError(f"secret pattern {token!r} detected in diff; aborting")
    if len(diffcontent) > 100 * 1024:
        raise RuntimeError(f"diff too large ({len(diffcontent)} bytes); aborting for safety")
    if len(changed_files) > 50:
        raise RuntimeError(f"too many files ({len(changed_files)}); aborting")
    # 提交
    commit_msg = (f"ci(auto): autopilot fix {tag}\n\n"
                  f"Triggered by CI failure patterns; details in job logs.\n")
    git("commit", "-q", "-m", commit_msg)
    git("push", "origin", BRANCH)
    print("[git] pushed autopilot fix to origin/main")
    return True

# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
def summarize_run(run: dict, jobs: List[dict]) -> Dict[str, int]:
    counts = {"success":0, "skipped":0, "failure":0, "cancelled":0,
              "running":0, "waiting":0, "pending":0, "other":0}
    for j in jobs:
        c = j.get("conclusion")
        s = j.get("status")
        if c in ("success", "skipped", "failure", "cancelled"):
            counts[c] += 1
        elif s in ("queued", "waiting", "requested", "pending"):
            counts["waiting"] += 1
        elif s == "in_progress":
            counts["running"] += 1
        else:
            counts["other"] += 1
    return counts

def main() -> int:
    print(f"[autopilot] repo={GITHUB_OWNER}/{GITHUB_REPO} branch={BRANCH} "
          f"max_rounds={MAX_ROUNDS}")
    last_seen_run_id = 0
    seen_errors_count: Dict[str, int] = {}  # hash(job_name+error_line_1st) -> n

    for round_i in range(1, MAX_ROUNDS + 1):
        print(f"\n=== Round {round_i}/{MAX_ROUNDS} at {time.strftime('%H:%M:%S')} ===")
        run = find_latest_run()
        if not run:
            print("  no runs yet; sleep")
            time.sleep(POLL_SECONDS); continue
        rid = run.get("id")
        name = run.get("name")
        url  = run.get("html_url")
        status = run.get("status")
        conclusion = run.get("conclusion")
        print(f"  run #{rid}: {name} status={status} conclusion={conclusion}")
        print(f"  {url}")

        jobs = list_jobs(rid)
        counts = summarize_run(run, jobs)
        print(f"  jobs: {counts}")

        # 没跑完 -> 等
        if status != "completed" and (counts["running"] + counts["waiting"]) > 0:
            if rid != last_seen_run_id:
                last_seen_run_id = rid
                print("  first sight of this run; will wait jobs to finish")
            # 简单等
            time.sleep(POLL_SECONDS); continue

        # 本次 run 全部完成
        succeeded = (counts["failure"] == 0 and counts["cancelled"] == 0)
        if succeeded:
            print(f"\n🎉 ALL JOBS SUCCESS / SKIPPED in run #{rid}. Autopilot done.")
            return 0

        # 失败了，挑第一个失败 job 做补丁（每次一个补丁组，保持 commit 粒度小）
        failed_jobs = [j for j in jobs if j.get("conclusion") in ("failure", "cancelled")]
        patched_any = False
        for job in failed_jobs[:3]:  # 每轮最多尝试修 3 个 job
            jname = job["name"]
            print(f"\n  Analyzing FAILED job: {jname}")
            tail = fetch_failed_log_tail(job)
            if not tail:
                print("    (no logs yet, will retry)")
                continue
            # 错误指纹
            first_err = next((ln for ln in tail.splitlines()
                              if any(k in ln for k in ("error:", "Error ", "FAILED"))),
                             tail.split("\n")[0])[:120]
            key = f"{jname}::{first_err}"
            seen_errors_count[key] = seen_errors_count.get(key, 0) + 1
            print(f"    fingerprint: {key!r} (seen {seen_errors_count[key]}x)")
            if seen_errors_count[key] >= 3:
                print("    same error >= 3 times, autopatch cannot fix; HUMAN NEEDED")
                # 写一个 summary
                continue
            if try_autopatch(job, tail):
                patched_any = True
                break

        if patched_any:
            try:
                pushed = commit_and_push_if_dirty(tag=f"round{round_i}")
                if not pushed:
                    print("[autopilot] marked applied but diff empty? skip.")
            except RuntimeError as e:
                print(f"[git] safety stop: {e}", file=sys.stderr)
                return 2
            # 等待下一轮 run 出现再开始监测
            print("  sleeping 90s for GitHub to queue new run...")
            time.sleep(90)
            continue

        # 走到这里 = 没修掉任何 job，或者需要人工介入。
        # 记录一份失败摘要到仓库根 CI_AUTOPILOT_REPORT.md，方便主人醒来直接看。
        report_lines = [
            "# CI Autopilot Report",
            "",
            f"- Generated at: {time.strftime('%Y-%m-%d %H:%M:%S')}",
            f"- Latest run: {url}",
            f"- Round: {round_i}/{MAX_ROUNDS}",
            "",
            "## Failed jobs requiring human review",
            ""
        ]
        for job in failed_jobs:
            report_lines.append(f"### {job['name']} (conclusion={job['conclusion']})")
            report_lines.append(f"- Job steps: {job.get('html_url', '')}")
            # 放 40 行核心
            logtail = fetch_failed_log_tail(job)
            snippet = "\n".join(logtail.splitlines()[:160])
            report_lines.append("")
            report_lines.append("```")
            report_lines.append(snippet)
            report_lines.append("```")
            report_lines.append("")
        report_lines.append("## Notes")
        report_lines.append("- Autopilot rules matched zero fixes for these failures.")
        report_lines.append("- If patterns repeat, add rules to "
                            "`scripts/ci_autopilot.py::RULES`.")
        with open(os.path.join(REPO_ROOT, "CI_AUTOPILOT_REPORT.md"), "w", encoding="utf-8") as f:
            f.write("\n".join(report_lines))
        try:
            commit_and_push_if_dirty(tag="needs-human")
        except RuntimeError as e:
            print(f"[git] cannot push report: {e}", file=sys.stderr)
        return 1

    print(f"[autopilot] exceeded MAX_ROUNDS={MAX_ROUNDS}; exit")
    return 1

if __name__ == "__main__":
    sys.exit(main())
