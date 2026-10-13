cmake_minimum_required(VERSION 3.19)
project(
        sts_lightspeed
        VERSION 0.1
        DESCRIPTION "High Performance, rng accurate implementation of Slay the Spire with tools for tree search and ml."
        LANGUAGES CXX)

file (GLOB_RECURSE sts_lightspeed_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/EHsc)
endif()

# GCC/Clang flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    string(APPEND CMAKE_CXX_FLAGS " -Wno-shift-count-overflow -O3")
elseif(MSVC)
    # MSVC: /O2 is equivalent to -O3 optimization level
    set(CMAKE_CXX_FLAGS_RELEASE "/O2")
endif()

add_subdirectory(json)
add_subdirectory(pybind11)

pybind11_add_module(slaythespire bindings/slaythespire.cpp collect/scenebuild.cpp collect/scenebuild_bind.cpp collect/action.cpp collect/encs_bind.cpp collect/zvalue.cpp collect/selfplay.cpp collect/teacher.cpp bindings/torch_eval.cpp bindings/bindings-util.cpp ${sts_lightspeed_SOURCES}
    collect/collect.h)

# ⚠️ Windows：torch 的预编译库是 **release CRT（/MD）**，而 Debug 配置默认 /MDd ⇒ 跨边界传
# `std::string` / `std::istream` / `at::Tensor` 这类对象是 UB（实测：`run_batch` 直接原生崩、
# 连 traceback 都没有）。只把这**一个**目标统一到 /MD（引擎另两个可执行文件不受影响）。
if(MSVC)
    set_target_properties(slaythespire PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

# ── 包化：`import slaythespire` 自己带上"先 import torch"这个副作用 ──
# 扩展静态链接 libtorch（`c10.dll` / `torch_cpu.dll`），而 Windows 的加载器在 `PyInit_*`
# **之前**就把导入表解析完了 ⇒ 库自己没机会登记 `torch/lib`；`/DELAYLOAD` 也做不到（从
# `c10.dll` 导入数据符号，链接器直接拒 + 见 `.agent/DIARY.md`）。⇒ 只能让**宿主**先跑一次
# `import torch`。做法：扩展改名 `_slaythespire` 放进同名包里（见
# `bindings/slaythespire_pkg/__init__.py` 的注释）。
# ⚠️ `OUTPUT_NAME` 必须与 `PYBIND11_MODULE` 的名字一致 —— `PyInit_<最后一段>` 的约定。
#
# ⚠️ **落点按生成器类型分**：多配置（MSVC）走 `build/<配置>/slaythespire/`；单配置
#    （远端 ninja/make，`CMAKE_BUILD_TYPE=Debug`）直接 `build/slaythespire/` ——
#    与改造前 `.so` 的落点**同层**，远端那个指向 `build/` 的 `.pth` 因此不用动。
if(CMAKE_CONFIGURATION_TYPES)
    set(_STS_PKG_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/slaythespire")
    set(_STS_PKG_OUT "${CMAKE_BINARY_DIR}/$<CONFIG>")
else()
    set(_STS_PKG_DIR "${CMAKE_BINARY_DIR}/slaythespire")
    set(_STS_PKG_OUT "${CMAKE_BINARY_DIR}")
endif()
set_target_properties(slaythespire PROPERTIES
        OUTPUT_NAME _slaythespire
        LIBRARY_OUTPUT_DIRECTORY "${_STS_PKG_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${_STS_PKG_DIR}")
# 包源码是仓库里的真文件（`INPUT` + `file(GENERATE)` = 拷进构建树，genex 会展开）
file(GENERATE OUTPUT "${_STS_PKG_DIR}/__init__.py"
        INPUT "${CMAKE_CURRENT_SOURCE_DIR}/bindings/slaythespire_pkg/__init__.py")
# 类型面：包 `__init__.pyi` 只做重导出，实体在 stubgen 生成的 `_slaythespire.pyi` 里。
file(GENERATE OUTPUT "${_STS_PKG_DIR}/__init__.pyi"
        INPUT "${CMAKE_CURRENT_SOURCE_DIR}/bindings/slaythespire_pkg/__init__.pyi")
# ⚠️ 清掉旧布局留下的 `slaythespire.pyi`（在包目录**旁边**）：CPython 运行时不看 `.pyi`，
#    但类型检查器会优先选它 ⇒ 留着就是一份永远不刷新的旧类型面。
#    （`file(REMOVE)` 不求值 genex ⇒ 用 GLOB 按两种布局扫。）
file(GLOB _sts_stale_pyi "${CMAKE_BINARY_DIR}/slaythespire.pyi"
        "${CMAKE_BINARY_DIR}/*/slaythespire.pyi"
        "${CMAKE_BINARY_DIR}/slaythespire/slaythespire"
        "${CMAKE_BINARY_DIR}/*/slaythespire/slaythespire")
file(REMOVE_RECURSE ${_sts_stale_pyi})

# .pyi
if(WIN32)
    set(_STS_VENV_PY "${CMAKE_SOURCE_DIR}/.venv/Scripts/python.exe")
else()
    set(_STS_VENV_PY "${CMAKE_SOURCE_DIR}/.venv/bin/python")
endif()

# ── libtorch（**必选**）：用本地 Python 环境那份 torch 的 C++ 头/库（pip wheel 自带，不必另下）──
# 配置：cmake -DTORCH_CMAKE_PREFIX=$(python -c "import torch;print(torch.utils.cmake_prefix_path)")
# ⚠️ 原先是"`STS_TORCH=ON` 才链"的可选开关，2026-10-11 起**一律链**（用户："何必要 STS_WITH_TORCH"）；
#    当时它存在的理由是"链上就要求先 import torch"—— 现在那个代价由上面那层包吃掉了。
if(TORCH_CMAKE_PREFIX)
    list(APPEND CMAKE_PREFIX_PATH "${TORCH_CMAKE_PREFIX}")
endif()
# ⚠️ CUDA 轮子（`+cuXXX`）在没有 nvcc 的机器上会**配置失败**：机器上残存的 `/usr/local/cuda*`
#    让 `Caffe2Config` 里的 `find_package(CUDA)` 成功 ⇒ 接着 `enable_language(CUDA)` 找不到编译器。
#    `Caffe2/public/cuda.cmake` 开头是"穷人版 include guard"：`if(TARGET torch::cudart) return()`
#    ⇒ 先占一个同名空 target，它立刻退出。本项目只用 CPU 侧（TorchScript），不需要 CUDA。
if(NOT MSVC)
    add_library(torch::cudart INTERFACE IMPORTED)
endif()
find_package(Torch REQUIRED)
if(Torch_FOUND)
    target_link_libraries(slaythespire PRIVATE ${TORCH_LIBRARIES})
    target_include_directories(slaythespire PRIVATE ${TORCH_INCLUDE_DIRS})
    # ⚠️ torch 的 wheel **自带一份 pybind11**（`torch/include/pybind11`）。它若排在子模块之前，
    #    `#include <pybind11/...>` 会全部命中 torch 那份（实测：torch 3.0.1 / 子模块 3.0.4）⇒
    #    同一个模块里两版 `function_record` 的布局与释放语义不一致，进程**退出时**在
    #    `cpp_function::destruct` 里 free 非法指针（Linux 直接 SIGABRT，退出码 134）。
    #    必须把子模块抬到最前，让整份模块只用一套 pybind11。
    target_include_directories(slaythespire SYSTEM BEFORE PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/pybind11/include")
    # ⚠️ 别想着给 c10.dll 加 /DELAYLOAD：模块从它导入了一个**数据**符号
    # （`caffe2::detail::std_string_metadata_index`），ld-link 直接拒绝
    # （"cannot delay-load ... due to import of data"）⇒ 链了 torch 的模块在 Windows 上
    # 就是**必须先 `import torch`**（它会把 torch/lib 加进 DLL 搜索路径）。
    message(STATUS "libtorch ${Torch_VERSION}：C++ 侧可直调网络")
else()
    message(STATUS "libtorch 未找到：只编 C++ 树（网络由 Python 前向）")
endif()
target_include_directories(slaythespire PUBLIC include)
target_include_directories(slaythespire PUBLIC json/single_include)
target_include_directories(slaythespire PUBLIC bindings)
# 采集侧（自对弈 / 教师 / 造局面 / 场景随机数）住在 `collect/` —— 与 `bindings/` 分开，因为
# 它们不参与"引擎绑定"那层语义（不进 `slaythespire.h` 的对外面），只是采集热循环。
target_include_directories(slaythespire PUBLIC collect)

if(EXISTS "${_STS_VENV_PY}")
    # `-o` 要给**包的父目录**（stubgen 会按点分模块名建子目录 ⇒ 落到
    # `<父>/slaythespire/_slaythespire.pyi`）。
    add_custom_command(
            TARGET slaythespire POST_BUILD
            COMMAND "${_STS_VENV_PY}" -c "import sys,torch,runpy; sys.argv=['pybind11_stubgen','slaythespire._slaythespire','-o']+sys.argv[1:]; runpy.run_module('pybind11_stubgen',run_name='__main__')" "${_STS_PKG_OUT}"
            COMMENT "refresh slaythespire/_slaythespire.pyi (pybind11_stubgen)")
else()
    message(WARNING "python no found: ${_STS_VENV_PY}")
endif()


add_executable(main apps/main.cpp ${sts_lightspeed_SOURCES})
target_link_directories(main PRIVATE json::nlohmann_json)
target_include_directories(main PUBLIC include)
target_include_directories(main PUBLIC json/include)

add_executable(test apps/test.cpp ${sts_lightspeed_SOURCES})
target_include_directories(test PUBLIC include)
target_link_directories(test PUBLIC json::nlohmann_json)
target_include_directories(test PUBLIC json/include)

add_executable(small-test apps/small-test.cpp bindings/bindings-util.cpp ${sts_lightspeed_SOURCES})

# 纯 C++ 的测试（造局面 / 引擎逻辑）—— 与 Python 那套 `_chk_*.py` 分开：被测对象与训练侧
# 毫无关系，写成 Python 只是多一层 pybind 往返 + 一层"忘了重编"的风险。
# 照 KataGo 的分法（`cpp/tests/test*.cpp` + `./katago runtests`）。
add_executable(scene-test apps/scene-test.cpp collect/scenebuild.cpp ${sts_lightspeed_SOURCES})
target_include_directories(scene-test PUBLIC include)
target_include_directories(scene-test PUBLIC collect)
target_link_directories(scene-test PRIVATE json::nlohmann_json)
target_include_directories(scene-test PUBLIC json/include)

# 引擎侧的 C++ 测试（动作空间 / 编码）。`bindings/bindings-util.cpp` 就是被测对象：
# 它**不拉 pybind 与 libtorch**（`small-test` 早就在链它）⇒ 这个目标编译很快、跑起来也没有
# 网络依赖。⚠️ 别把 `bindings/slaythespire.cpp` 加进来 —— 那个 TU 带 `PYBIND11_MODULE`。
add_executable(engine-test apps/engine-test.cpp collect/action.cpp bindings/bindings-util.cpp collect/scenebuild.cpp ${sts_lightspeed_SOURCES})

# 自对弈热循环的 C++ 测试（`collect/selfplay.cpp` 的驱动 + 前向服务）——**链 torch、不带 pybind**。
# ⚠️ 为什么单独一个目标：它要 `TorchEvaluator`（libtorch）与注入的 `write_obs`（`snapshot.h`），
#    而 `engine-test` 刻意不链 torch（保持秒级构建）。
# ⚠️ 运行方式与 `slaythespire` 同病：进程里**必须先 `import torch`**（DLL 搜索路径），
#    所以本测试**从 Python 侧拉起**（`_chk_all` 的 engine 档），或手工 `python -c "import torch; ..."`。
add_executable(selfplay-test apps/selfplay-test.cpp collect/action.cpp collect/selfplay.cpp
        collect/scenebuild.cpp collect/zvalue.cpp bindings/torch_eval.cpp ${sts_lightspeed_SOURCES})
target_include_directories(selfplay-test PUBLIC include)
target_include_directories(selfplay-test PUBLIC collect)
target_include_directories(selfplay-test PUBLIC bindings)
target_include_directories(selfplay-test PUBLIC json/single_include)
target_include_directories(selfplay-test PUBLIC json/include)
target_link_directories(selfplay-test PRIVATE json::nlohmann_json)
if(Torch_FOUND)
    target_link_libraries(selfplay-test PRIVATE ${TORCH_LIBRARIES})
    target_include_directories(selfplay-test PRIVATE ${TORCH_INCLUDE_DIRS})
    # ⚠️ `torch/include/pybind11/conduit/wrap_include_python_h.h` 要 `Python.h` —— 那是 torch 的
    #    **传递依赖**（它自带 pybind11）。`slaythespire` 那侧由 `pybind11_add_module` 自动满足，
    #    本目标不是 pybind 模块 ⇒ 手工把 venv 解释器的头目录加进来。
    execute_process(COMMAND "${_STS_VENV_PY}" -c "import sysconfig;print(sysconfig.get_paths()['include'])"
            OUTPUT_VARIABLE _STS_PY_INC OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(EXISTS "${_STS_PY_INC}")
        target_include_directories(selfplay-test PRIVATE "${_STS_PY_INC}")
    else()
        message(WARNING "selfplay-test：找不到 Python 头目录（torch 的头要 Python.h）")
    endif()
    # ⚠️ 还要**链** `python3xx.lib`：torch 的 `torch_python.lib` 依赖它，而本目标不是 pybind
    #    模块（`pybind11_add_module` 会自动带上）。位置与头目录同一棵树：`<prefix>/libs`。
    execute_process(COMMAND "${_STS_VENV_PY}" -c "import sysconfig;print(sysconfig.get_config_var('installed_base') or sysconfig.get_paths()['prefix'])"
            OUTPUT_VARIABLE _STS_PY_BASE OUTPUT_STRIP_TRAILING_WHITESPACE)
    file(GLOB _STS_PY_LIB "${_STS_PY_BASE}/libs/python3*.lib" "${_STS_PY_BASE}/lib/libpython3*")
    if(_STS_PY_LIB)
        target_link_directories(selfplay-test PRIVATE "${_STS_PY_BASE}/libs" "${_STS_PY_BASE}/lib")
        target_link_libraries(selfplay-test PRIVATE ${_STS_PY_LIB})
    else()
        message(WARNING "selfplay-test：找不到 Python 导入库（torch_python 要它）")
    endif()
endif()
# 与 `slaythespire` 同：torch 的 wheel 自带 pybind11，本目标不 include pybind 那层，无所谓；
# 但 CRT 必须与 torch 一致（/MD）—— 跨边界传 `std::string` / `torch::Tensor` 的对象布局。
set_target_properties(selfplay-test PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
target_include_directories(engine-test PUBLIC include)
target_include_directories(engine-test PUBLIC collect)
target_include_directories(engine-test PUBLIC bindings)
target_link_directories(engine-test PRIVATE json::nlohmann_json)
target_include_directories(engine-test PUBLIC json/include)
target_link_directories(small-test PRIVATE json::nlohmann_json)
target_include_directories(small-test PUBLIC include)
target_include_directories(small-test PUBLIC json/include)
target_include_directories(small-test PUBLIC bindings)
# `bindings/bindings-util.cpp` 拉 `slaythespire.h`，而后者 `#include "snapshot.h"` —— 那个头
# 住在 `collect/`（与 `slaythespire` 目标同款）⇒ 这里也得有，否则 `small-test` 编不过。
target_include_directories(small-test PUBLIC collect)

