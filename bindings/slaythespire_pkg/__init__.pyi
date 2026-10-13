# 包对外的类型面：扩展的全部公开名字（`__all__` 由 stubgen 写在 `_slaythespire.pyi` 里，
# `import *` 因此正好带出全部 —— 不用手抄第二份名单）。
# ⚠️ 用**绝对**名字（不用 `from ._slaythespire import *`）：本文件在源码树里的那份旁边没有
#    `_slaythespire`（构建产物），相对导入会被 Pylance 按"本文件目录"解析而报错。
from slaythespire._slaythespire import *
