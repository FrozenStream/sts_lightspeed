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

# ── 包落点（**必须先于 `pybind11_add_module` 之外的一切 target 操作**，但它在下面：
#    这里只把变量算出来）──────────────────────────────────────────────────────────
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
# ⚠️ `file(REMOVE_RECURSE)` **没有实参**会当场报 "file must be called with at least two arguments"
#    ⇒ 全新克隆（构建树里还没有任何旧 `.pyi`）必然配置失败。Empty GLOB = 本来就没事要干。
if(_sts_stale_pyi)
    file(REMOVE_RECURSE ${_sts_stale_pyi})
endif()

# .pyi
if(WIN32)
    set(_STS_VENV_PY "${CMAKE_SOURCE_DIR}/.venv/Scripts/python.exe")
else()
    set(_STS_VENV_PY "${CMAKE_SOURCE_DIR}/.venv/bin/python")
endif()
# `selfplay-test` 要 **Python 头/库**吗？不要 —— C++ 侧不链 libtorch（`net_eval.h` → ONNX Runtime），
# 而 `collect/selfplay.h` 也不碰 pybind11（绑定层单独一个 TU：`collect/selfplay_bind.cpp`）
# ⇒ 那个目标只依赖 `onnxruntime.dll`。
# ⚠️ 下面这个 `_STS_PY_HOST` 只给**刷桩**那条命令用（`pybind11_stubgen` 要在能 `import slaythespire`
#    的解释器里跑）；本机 = 仓库 venv，**远端（conda）没有 venv** ⇒ 退回 `-DPython_EXECUTABLE=` 那个。
set(_STS_PY_HOST "${_STS_VENV_PY}")
if(NOT EXISTS "${_STS_PY_HOST}" AND Python_EXECUTABLE)
    set(_STS_PY_HOST "${Python_EXECUTABLE}")
endif()

# ── 推理后端 = ONNX Runtime（**唯一引擎**，2026-10-15 定案）──────────────────────────────
#
# 为什么要它（而不是 libtorch/TorchScript）：链 libtorch 的代价是**全链路**的 —— `.pyd` 静态链 `c10.dll`/`torch_cpu.dll`，
# 于是 ① 进程里必须先 `import torch`（Windows 的加载器在 `PyInit_*` **之前**就解析完导入表）
# ② 构建要 `TORCH_CMAKE_PREFIX` ③ 测试可执行文件还得挂 Python 头/库。换成 ORT 之后这些依赖一条不剩（只留 `onnxruntime.dll`）。
# ⚠️ 换后端 = 换**数值**（ORT 的算子不是 torch 的实现 ⇒ 不可能逐位）：历史对账记录（同权重 argmax 逐行一致、
#    `max|Δ| ≈ 1e-6`）在 `.agent/notes.md` §3。旧实现（`torch_eval.*`）与 TorchScript 导出器（`tsnet.py`）已删 —— 要再对账就从 git 历史取。
#
# 要的库：`third_party/onnxruntime/{include,lib,bin}`（官方包各自解包，**不进 git**；
# 也不进 `_push.sh` 的清单 ⇒ 远端单独下一份，见 `_ort.sh`）。用 `-DORT_HOME=` 指别处。
# ⚠️ 官方包分 CPU / GPU(cu12|cu13)：**头与导入库是同一套**，差别只在运行时要不要
#    `onnxruntime_providers_cuda`（`AppendExecutionProvider_CUDA` 按名字找）。选错包 = 只能跑 CPU。
if(NOT ORT_HOME)
    set(ORT_HOME "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")
endif()
find_path(ORT_INCLUDE_DIR onnxruntime_cxx_api.h PATHS "${ORT_HOME}/include" NO_DEFAULT_PATH)
find_library(ORT_LIB onnxruntime PATHS "${ORT_HOME}/lib" NO_DEFAULT_PATH)
if(NOT ORT_INCLUDE_DIR OR NOT ORT_LIB)
    message(FATAL_ERROR "找不到 ONNX Runtime（期望 ${ORT_HOME}/{include,lib}）—— "
            "解包 onnxruntime-<平台>-1.31.0 到 third_party/onnxruntime，或 -DORT_HOME=<别处>")
endif()
message(STATUS "推理后端 = ONNX Runtime：${ORT_LIB}")
# 链接 + 把 DLL 拷到产物旁边（Windows 上 `onnxruntime.dll` 没在 PATH 里就加载失败）。
# ⚠️ 官方包的布局两头不一：**Windows 的 DLL 在 `lib/`**（与 `.lib` 同处），Linux 的也在 `lib/`。
function(sts_link_net tgt)
    target_link_libraries(${tgt} PRIVATE "${ORT_LIB}")
    target_include_directories(${tgt} SYSTEM PRIVATE "${ORT_INCLUDE_DIR}")
    if(WIN32)
        set(_sts_ort_dll "")
        foreach(_cand "${ORT_HOME}/lib/onnxruntime.dll" "${ORT_HOME}/bin/onnxruntime.dll")
            if(EXISTS "${_cand}")
                set(_sts_ort_dll "${_cand}")
            endif()
        endforeach()
        if(_sts_ort_dll)
            add_custom_command(TARGET ${tgt} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${_sts_ort_dll}" "$<TARGET_FILE_DIR:${tgt}>")
        else()
            message(WARNING "找不到 onnxruntime.dll（${ORT_HOME}）—— 运行时会加载失败")
        endif()
    endif()
endfunction()

# 后端源文件：**同一个调用面**（`collect/net_eval.h` 里的 `NetEvaluator` + `NetOut`）。
set(_STS_NET_SRC "collect/onnx_eval.cpp")

# 三个调用目标共用同一组自对弈模块，故障测试仅替换推理器实现。
set(_STS_SELFPLAY_SRC collect/selfplay.cpp collect/selfplay_tree.cpp collect/selfplay_forward.cpp
        collect/selfplay_report.cpp collect/collect_util.cpp)

pybind11_add_module(slaythespire bindings/slaythespire.cpp collect/scenebuild.cpp collect/scenebuild_bind.cpp collect/action.cpp collect/zvalue.cpp ${_STS_SELFPLAY_SRC} collect/selfplay_bind.cpp collect/teacher.cpp collect/teacher_bind.cpp collect/eval.cpp ${_STS_NET_SRC} bindings/bindings-util.cpp ${sts_lightspeed_SOURCES}
    collect/collect.h)
sts_link_net(slaythespire)

# 推理后端的独立探针（只做"载入 → 跑批 → 报读数/耗时"，**不碰搜索热循环**）：
#   · 这一层出问题（图/设备/线程数/输出宽度）时，用它能**不经过搜索**就定位；
#   · `--dump` 写原始 float32 ⇒ 换 ORT 版本 / 换执行提供者时可以逐批对账（同版本应逐位，跨版本容差级）。
add_executable(net-test apps/net-test.cpp ${_STS_NET_SRC})
target_include_directories(net-test PUBLIC include)
target_include_directories(net-test PUBLIC collect)
target_include_directories(net-test PUBLIC bindings)
sts_link_net(net-test)

# ── 包化：扩展改名 `_slaythespire` 放进同名包（`bindings/slaythespire_pkg/__init__.py`）──
# 为什么有这层：包是**类型面**与导入名的落点（`_slaythespire.pyi` 由 stubgen 生成在包里；
# `import slaythespire` 拿到包）。⚠️ 包内那段"先 `import torch`"的 shim 已随 libtorch 一起删
# （见 `bindings/slaythespire_pkg/__init__.py`）。
# ⚠️ `OUTPUT_NAME` 必须与 `PYBIND11_MODULE` 的名字一致 —— `PyInit_<最后一段>` 的约定。
set_target_properties(slaythespire PROPERTIES
        OUTPUT_NAME _slaythespire
        LIBRARY_OUTPUT_DIRECTORY "${_STS_PKG_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${_STS_PKG_DIR}")
# 清掉旧布局留下的 `slaythespire.pyi`（在包目录**旁边**）：CPython 运行时不看 `.pyi`，但类型检查器
# 会优先选它 ⇒ 留着就是一份永远不刷新的旧类型面。（`file(REMOVE)` 不求值 genex ⇒ 用 GLOB 扫。）
file(GLOB _sts_stale_pyi "${CMAKE_BINARY_DIR}/slaythespire.pyi"
        "${CMAKE_BINARY_DIR}/*/slaythespire.pyi"
        "${CMAKE_BINARY_DIR}/slaythespire/slaythespire"
        "${CMAKE_BINARY_DIR}/*/slaythespire/slaythespire")
# ⚠️ `file(REMOVE_RECURSE)` **没有实参**会当场报 "file must be called with at least two arguments"
#    ⇒ 全新克隆（构建树里还没有任何旧 `.pyi`）必然配置失败。Empty GLOB = 本来就没事要干。
if(_sts_stale_pyi)
    file(REMOVE_RECURSE ${_sts_stale_pyi})
endif()

# ⚠️ Windows：ORT 的预编译库是 **release CRT（/MD）**，而 Debug 配置默认 /MDd ⇒ 跨边界传
# `std::string` / `Ort::Value` 这类对象是 UB（实测过：原生崩、连 traceback 都没有）。
# 只把这**一个**目标统一到 /MD（另外几个可执行文件不受影响）。
if(MSVC)
    set_target_properties(slaythespire PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
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
            COMMAND "${_STS_VENV_PY}" -c "import sys,runpy; sys.argv=['pybind11_stubgen','slaythespire._slaythespire','-o']+sys.argv[1:]; runpy.run_module('pybind11_stubgen',run_name='__main__')" "${_STS_PKG_OUT}"
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

# 自对弈热循环的 C++ 测试（池化调度 + 单树搜索 + 前向服务）——**不带 pybind、也不链 Python**。
# ⚠️ 为什么单独一个目标：它要 `NetEvaluator`（前向）与 `zvalue`（值口径），
#    而 `engine-test` 刻意不连网络（保持秒级构建）。
# ⚠️ ONNX 之后它不再需要 Python 头/库（旧 libtorch 的头会拉 `Python.h`，而 `pyconfig.h` 自带
#    `#pragma comment(lib,"python3XX.lib")`）⇒ 直接跑 `selfplay-test.exe <net.onnx>` 即可。
add_executable(selfplay-test apps/selfplay-test.cpp collect/action.cpp ${_STS_SELFPLAY_SRC}
        collect/scenebuild.cpp collect/zvalue.cpp ${_STS_NET_SRC} ${sts_lightspeed_SOURCES})
target_include_directories(selfplay-test PUBLIC include)
target_include_directories(selfplay-test PUBLIC collect)
target_include_directories(selfplay-test PUBLIC bindings)
target_include_directories(selfplay-test PUBLIC json/single_include)
target_include_directories(selfplay-test PUBLIC json/include)
target_link_directories(selfplay-test PRIVATE json::nlohmann_json)
sts_link_net(selfplay-test)
# ⚠️ CRT 必须与 ORT 一致（/MD）：跨边界传 `std::string` / `Ort::Value` 这类对象时，
# 布局与释放语义在 /MD 与 /MDd 之间不兼容（`sts_link_net` 里已设，这里只是显式说明）。
set_target_properties(selfplay-test PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# 用可控推理器覆盖启动、前向、补场和交付失败；不链接 ONNX Runtime。
# 同时覆盖教师采集的有序交付，以及 worker / 写盘失败后的线程收尾。
add_executable(selfplay-failure-test apps/selfplay-failure-test.cpp collect/action.cpp ${_STS_SELFPLAY_SRC}
        collect/scenebuild.cpp collect/zvalue.cpp collect/teacher.cpp ${sts_lightspeed_SOURCES})
target_include_directories(selfplay-failure-test PRIVATE include collect bindings json/single_include)
set_target_properties(selfplay-failure-test PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
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

# ── 整局测试：战斗用网络、非战斗随机 ────────────────────────────────────────────────
# 与 `selfplay-test` 共用同一组源（网络前向 + 搜索 + 动作层），但驱动的是**整局状态机**
# （`GameContext`）：Neow → 地图 → 战斗/事件/商店/营火 → … → Boss。战斗仍走 `collect::selfplay`
# 的 MCTS（生产同一条），非战斗走 `GameAction::getAllActionsInState` 的均匀随机。
# ⚠️ 它需要 `sim/search/GameAction.cpp` —— 那个 TU 在 `sts_lightspeed_SOURCES` 的 glob 里
#    （`src/**/*.cpp`），所以不用单列。
add_executable(fullrun-test apps/fullrun-test.cpp collect/action.cpp ${_STS_SELFPLAY_SRC}
        collect/scenebuild.cpp collect/zvalue.cpp ${_STS_NET_SRC} ${sts_lightspeed_SOURCES})
target_include_directories(fullrun-test PUBLIC include)
target_include_directories(fullrun-test PUBLIC collect)
target_include_directories(fullrun-test PUBLIC bindings)
target_include_directories(fullrun-test PUBLIC json/single_include)
target_include_directories(fullrun-test PUBLIC json/include)
target_link_directories(fullrun-test PRIVATE json::nlohmann_json)
sts_link_net(fullrun-test)
set_target_properties(fullrun-test PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

