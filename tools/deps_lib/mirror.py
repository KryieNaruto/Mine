"""GitHub 源码拉取国内镜像层。

ghproxy 类加速服务把 `<前缀> + https://github.com/<repo>` 原样透传到 GitHub。
测速选可达且最快的镜像前缀;全部不可达返回 None(调用方退回官方直连)。
测速结果不缓存/不落盘——每次运行时现测,避免陈旧选择(CMakeCache 陈旧教训)。
"""
from __future__ import annotations

import time
import urllib.request

# ghproxy 类加速前缀。失效/新增在此维护(测速会自动跳过不可达的)。
MIRROR_PREFIXES = ["https://ghproxy.net/", "https://gh-proxy.com/"]

# 探测目标:任意一个 GitHub 仓库的 git smart-http 端点(镜像会原样透传)。
# 用 glm 而非大仓库,探测请求尽量小。
_PROBE_URL = "https://github.com/g-truc/glm.git/info/refs?service=git-upload-pack"


def mirror_url(repo_url: str, prefix: str | None) -> str:
    """prefix 非空时给 repo_url 前挂镜像前缀,否则原样返回。"""
    return prefix + repo_url if prefix else repo_url


def _probe(prefix: str, timeout: float) -> float | None:
    """探测某前缀是否可达且能透传 git 数据,返回耗时秒数;不可达返回 None。"""
    url = prefix + _PROBE_URL
    try:
        start = time.monotonic()
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            resp.read()  # 触发实际请求
        return time.monotonic() - start
    except Exception:
        return None


def pick_mirror_prefix(timeout: float = 6.0) -> str | None:
    """返回当前可达且最快的镜像前缀;全部不可达返回 None。"""
    best = None
    best_t = None
    for p in MIRROR_PREFIXES:
        t = _probe(p, timeout)
        if t is not None and (best_t is None or t < best_t):
            best, best_t = p, t
    return best
