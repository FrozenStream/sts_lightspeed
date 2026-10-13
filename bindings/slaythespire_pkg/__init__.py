"""引擎扩展（`_slaythespire`）的宿主包 —— 这个文件只有一件事：把 DLL 目录在加载**之前**登记好。

⚠️ **为什么必须有这一层**：扩展 `.pyd` 静态链接了 libtorch（`c10.dll` / `torch_cpu.dll`），
而 Windows 的加载器在 `PyInit__slaythespire` **之前**就把它的导入表解析完了 —— 库自己
**没有**任何机会去登记搜索路径（`DllMain` 也在那之后）。`import torch` 之所以能修好，
纯粹是它的副作用：`torch/__init__.py` 会
`os.add_dll_directory(<site-packages>/torch/lib)`，而那两个 DLL 正好住在那里。

⇒ 把 `import torch` 放进**包的 `__init__`**，`import slaythespire` 就自带这个副作用，
   调用侧不必再记"必须先 import torch"（原先是散在 18 个脚本里的一行 + 一句注释，漏一个就
   是 `ImportError: DLL load failed while importing slaythespire`）。

⚠️ `/DELAYLOAD` 这条路**做不到**（已实测，见 `.agent/DIARY.md`）：本模块从 `c10.dll` 导入了
   **数据**符号（`caffe2::detail::std_string_metadata_index`），链接器直接拒绝
   （`cannot delay-load ... due to import of data`）。
⚠️ 别把 `import torch` 挪到本文件里的 `from ._slaythespire import *` **之后** —— 那样就白写了。
⚠️ Linux 上大概率不需要它（`.so` 的 RPATH 自己能解决），留着无害：多花一次 torch 的导入时间，
   而所有调用方本来就要 torch。
"""
import torch                       # noqa: F401  —— 只为副作用：登记 `torch/lib` 为 DLL 目录
# ⚠️ 用**绝对**名字而不是 `from ._slaythespire import *`：本文件有两个副本 —— 源码树里的这份
#    旁边**没有** `_slaythespire`（它是构建产物），相对导入会被静态检查按"本文件所在目录"解析
#    ⇒ Pylance 报「无法解析导入 `._slaythespire`」（而构建树里那份不报，因为它旁边就有
#    `_slaythespire.pyi`）。绝对名字走 `python.analysis.extraPaths`（本仓 = `./build/Debug`）
#    ⇒ 源码树那份也能解析到扩展的桩；运行时两种写法等价（父包此刻已在 `sys.modules` 里）。
from slaythespire._slaythespire import *       # noqa: F401,F403  —— 扩展的全部公开名字
