"""deps_lib —— 工作空间依赖管理共享模块。"""
import os

# Mine 根:tools/deps_lib/__init__.py -> tools/deps_lib -> tools -> Mine
MINE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

__all__ = ["MINE_ROOT"]
