"""引擎扩展（`_slaythespire`）的宿主包。

⚠️ 这里**曾经**有一段"先 `import torch`"的 shim，那是为 C++ 侧链 libtorch 的构建准备的：
`.pyd` 静态链 `c10.dll`/`torch_cpu.dll`，而 Windows 的加载器在 `PyInit__slaythespire` **之前**就把
导入表解析完，库自己没机会登记 DLL 搜索路径（`DllMain` 也在那之后）—— 只有 `import torch` 的副作用
（`os.add_dll_directory(<site-packages>/torch/lib)`）能修好，而 `/DELAYLOAD` 走不通（旧模块从
`c10.dll` 导入了**数据**符号，链接器直接拒绝）。

2026-10-15 起 C++ 侧只链 **ONNX Runtime**（`bindings/net_eval.h`）⇒ 那段 shim 连同 libtorch 一起删了：
`import slaythespire` 现在只依赖 `onnxruntime.dll`（CMake 会把它拷到扩展旁边）。

⚠️ 本文件有两个副本：源码树里的这份旁边**没有** `_slaythespire`（它是构建产物），构建树里的那份
    旁边才有（含 stubgen 生成的 `_slaythespire.pyi`）。下面用**绝对**名字导入而不是
    `from ._slaythespire import *` —— 相对导入会被静态检查按"本文件所在目录"解析，源码树那份
    于是报「无法解析导入 `._slaythespire`」；绝对名字走 `python.analysis.extraPaths`
    ⇒ 源码树那份也能解析到扩展的桩。运行时两种写法等价（父包此刻已在 `sys.modules` 里）。
"""
from slaythespire._slaythespire import *       # noqa: F401,F403  —— 扩展的全部公开名字