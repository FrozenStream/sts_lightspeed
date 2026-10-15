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
# `selfplay-test` 要 Python 头/库（torch 的头反过来要 `Python.h`）：本机 = 仓库 venv；
# **远端（conda）没有 venv** ⇒ 用 `-DPython_EXECUTABLE=` 那个解释器（`find_package(Python)` 找到的）。
# ⚠️ 只给 `selfplay-test` 用：刷桩那条命令在 `/bin/sh` 下引号很脆（`-c "…'…'…"` 会被 dash 截断），
#    而且远端不需要 `.pyi` ⇒ 刷桩**仍旧只在有 venv 时做**（远端手工刷）。
set(_STS_PY_HOST "${_STS_VENV_PY}")
if(NOT EXISTS "${_STS_PY_HOST}" AND Python_EXECUTABLE)
    set(_STS_PY_HOST "${Python_EXECUTABLE}")
endif()

# ── 推理后端（**过渡期两条路都能编**）：`STS_ONNX=ON` 用 ONNX Runtime，否则用旧的 libtorch ──
#
# 为什么留开关：换后端会换**数值**（ORT 的算子不是 torch 的实现 ⇒ 不能逐位）⇒ 过渡期必须能
# 同一份权重在两条路上跑、比"逐帧动作一致率/帧数/速度"。验证通过后（Stage 4）把默认翻过来、
# 再删掉 torch 那一支。
#
# `STS_ONNX=ON` 要的库：`third_party/onnxruntime/{include,lib,bin}`（本地各自解包官方包，**不进 git**；
# 也不进 `_push.sh` 的清单 ⇒ 远端单独下一份）。用 `-DORT_HOME=` 指别处（远端那份 cu13 包）。
# ⚠️ 官方包分 CPU / GPU(cu12|cu13)：**头与导入库是同一套**，差别只在运行时要不要
#    `onnxruntime_providers_cuda`（`AppendExecutionProvider_CUDA` 按名字找）。选错包 = 只能跑 CPU。
option(STS_ONNX "C++ 侧推理用 ONNX Runtime（关 = 旧的 libtorch/TorchScript）" OFF)

if(STS_ONNX)
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
    # ⚠️ 这个宏是**唯一**的后端开关：它同时决定 ① 编哪个 `*_eval.cpp` ② `net_eval.h` 选哪个类型
    #    ③ `slaythespire.NET_BACKEND` 报什么给 Python（`netarchive.py` 靠它挑导出器）。
    add_compile_definitions(STS_ONNX)
    # 链接 + 把 DLL 拷到产物旁边（Windows 上 `onnxruntime.dll` 没在 PATH 里就加载失败）。
    # ⚠️ 官方包的布局两头不一：**Windows 的 DLL 在 `lib/`**（与 `.lib` 同处），Linux 的在 `lib/` 也是。
    function(sts_link_net tgt)
        target_link_libraries(${tgt} PRIVATE "${ORT_LIB}")
        target_include_directories(${tgt} SYSTEM PRIVATE "${ORT_INCLUDE_DIR}")
        # ⚠️ 非 pybind 模块的目标（`selfplay-test`）也要 `collect/selfplay.h` 里的
        #    `<pybind11/pybind11.h>`（`registerSelfplay` 的签名）—— torch 时代那份是**蹭** torch
        #    自带的 pybind11 拿到的；现在显式用子模块那份（同一份，别让它俩打架）。
        target_link_libraries(${tgt} PRIVATE pybind11::headers)
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
    # Windows：ORT 的预编译库是 **release CRT（/MD）**，Debug 配置默认 /MDd ⇒ 跨边界传
    # `std::string` / `Ort::Value` 这类对象是 UB（torch 时代实测过：直接原生崩、无 traceback）
    # ⇒ 把两个链它的目标统一到 /MD。
    set(_STS_NET_CRT "MultiThreadedDLL")
else()
    # ── libtorch（旧路，过渡期保留）：用本地 Python 环境那份 torch 的 C++ 头/库（pip wheel 自带）──
    # 配置：cmake -DTORCH_CMAKE_PREFIX=$(python -c "import torch;print(torch.utils.cmake_prefix_path)")
    if(TORCH_CMAKE_PREFIX)
        list(APPEND CMAKE_PREFIX_PATH "${TORCH_CMAKE_PREFIX}")
    endif()
    # ⚠️ CUDA 轮子（`+cuXXX`）在没有 nvcc 的机器上会**配置失败**：机器上残存的 `/usr/local/cuda*`
    #    让 `Caffe2Config` 里的 `find_package(CUDA)` 成功 ⇒ 接着 `enable_language(CUDA)` 找不到编译器。
    #    `Caffe2/public/cuda.cmake` 开头是"穷人版 include guard"：`if(TARGET torch::cudart) return()`。
    if(NOT MSVC)
        add_library(torch::cudart INTERFACE IMPORTED)
    endif()
    find_package(Torch REQUIRED)
    message(STATUS "推理后端 = libtorch ${Torch_VERSION}")
    # ⚠️ torch 的 wheel **自带一份 pybind11**（`torch/include/pybind11`）。它若排在子模块之前，
    #    `#include <pybind11/...>` 会全部命中 torch 那份（实测：torch 3.0.1 / 子模块 3.0.4）⇒
    #    同一个模块里两版 `function_record` 的布局与释放语义不一致，进程**退出时**在
    #    `cpp_function::destruct` 里 free 非法指针（Linux 直接 SIGABRT，退出码 134）。
    #    必须把子模块抬到最前，让整份模块只用一套 pybind11。
    function(sts_link_net tgt)
        target_link_libraries(${tgt} PRIVATE ${TORCH_LIBRARIES})
        target_include_directories(${tgt} PRIVATE ${TORCH_INCLUDE_DIRS})
        target_include_directories(${tgt} SYSTEM BEFORE PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/pybind11/include")
        set_property(TARGET ${tgt} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    endfunction()
    # ⚠️ 别想着给 c10.dll 加 /DELAYLOAD：模块从它导入了一个**数据**符号
    #    （`caffe2::detail::std_string_metadata_index`），ld-link 直接拒绝
    #    （"cannot delay-load ... due to import of data"）⇒ 链了 torch 的模块在 Windows 上
    #    就是**必须先 `import torch`**（它会把 torch/lib 加进 DLL 搜索路径）。
    set(_STS_NET_CRT "MultiThreadedDLL")
endif()

# 后端选源文件：**同一个调用面**（`bindings/net_eval.h` 里的 `NetEvaluator` + `NetOut`），两个实现。
set(_STS_NET_SRC "bindings/torch_eval.cpp")
if(STS_ONNX)
    set(_STS_NET_SRC "bindings/onnx_eval.cpp")
endif()

pybind11_add_module(slaythespire bindings/slaythespire.cpp collect/scenebuild.cpp collect/scenebuild_bind.cpp collect/action.cpp collect/zvalue.cpp collect/selfplay.cpp collect/selfplay_bind.cpp collect/teacher.cpp collect/eval.cpp ${_STS_NET_SRC} bindings/bindings-util.cpp ${sts_lightspeed_SOURCES}
    collect/collect.h)
sts_link_net(slaythespire)

# 推理后端的独立探针（只做"载入 → 跑批 → 报读数/耗时"，**不碰搜索热循环**）：
#   · 换后端要验的就是这一层，把它绑在 `selfplay`/搜索结果上会让"热循环正在被改"挡住验证；
#   · 两种后端编两次同一个源 ⇒ `--dump` 的文件可直接对账（同权重同输入，期望 max|Δ| ~ 1e-7）。
add_executable(net-test apps/net-test.cpp ${_STS_NET_SRC})
target_include_directories(net-test PUBLIC include)
target_include_directories(net-test PUBLIC collect)
target_include_directories(net-test PUBLIC bindings)
sts_link_net(net-test)

# ── 包化：扩展改名 `_slaythespire` 放进同名包（`bindings/slaythespire_pkg/__init__.py`）──
# 为什么有这层：旧的 libtorch 后端下 `.pyd` 静态链 `c10.dll`/`torch_cpu.dll`，而 Windows 的加载器
# 在 `PyInit_*` **之前**就解析完导入表 ⇒ 只能让宿主先 `import torch`（那个 shim 就住在包 `__init__`）。
# ⚠️ 走 ONNX 后端（`STS_ONNX=ON`）时这层**不再必需**，但仍然留着：包名/桩路径是既有约定。
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
# `std::string` / `Ort::Value` 这类对象是 UB（在 torch 时代实测过：`run_batch` 直接原生崩、
# 连 traceback 都没有）。只把这**一个**目标统一到 /MD（另外几个可执行文件不受影响）。
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
        collect/scenebuild.cpp collect/zvalue.cpp ${_STS_NET_SRC} ${sts_lightspeed_SOURCES})
target_include_directories(selfplay-test PUBLIC include)
target_include_directories(selfplay-test PUBLIC collect)
target_include_directories(selfplay-test PUBLIC bindings)
target_include_directories(selfplay-test PUBLIC json/single_include)
target_include_directories(selfplay-test PUBLIC json/include)
target_link_directories(selfplay-test PRIVATE json::nlohmann_json)
sts_link_net(selfplay-test)
# ⚠️ CRT 必须与后端一致（/MD）：跨边界传 `std::string` / `Ort::Value` / `torch::Tensor` 这类对象时，
# 布局与释放语义在 /MD 与 /MDd 之间不兼容（`sts_link_net` 里已按后端设过，这里只是显式说明）。
set_target_properties(selfplay-test PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
# ⚠️ **两种后端都要**：`Python.h` 链进来的 `pyconfig.h` 自己带
#    `#pragma comment(lib,"python3XX.lib")`（见 uv 的 cpython `Include/pyconfig.h`）⇒ 只要哪个 TU
#    碰了 pybind11/Python 头，**链接期**就会找这个导入库。本目标不是 pybind 模块
#    （`pybind11_add_module` 会替我们链好），所以手工加：头目录 + 导入库所在目录 + 库名。
execute_process(COMMAND "${_STS_PY_HOST}" -c "import sysconfig;print(sysconfig.get_paths()['include'])"
        OUTPUT_VARIABLE _STS_PY_INC OUTPUT_STRIP_TRAILING_WHITESPACE)
if(EXISTS "${_STS_PY_INC}")
    target_include_directories(selfplay-test PRIVATE "${_STS_PY_INC}")
else()
    message(WARNING "selfplay-test：找不到 Python 头目录（pybind11 要 Python.h）")
endif()
# 位置与头目录同一棵树：glibc 的 `libpython3.12.so` 在 `<prefix>/lib`；Windows 的 `python312.lib`
# 在 `<prefix>/libs`。
execute_process(COMMAND "${_STS_PY_HOST}" -c "import sysconfig;print(sysconfig.get_config_var('installed_base') or sysconfig.get_paths()['prefix'])"
        OUTPUT_VARIABLE _STS_PY_BASE OUTPUT_STRIP_TRAILING_WHITESPACE)
file(GLOB _STS_PY_LIB "${_STS_PY_BASE}/libs/python3*.lib" "${_STS_PY_BASE}/lib/libpython3*")
if(_STS_PY_LIB)
    target_link_directories(selfplay-test PRIVATE "${_STS_PY_BASE}/libs" "${_STS_PY_BASE}/lib")
    target_link_libraries(selfplay-test PRIVATE ${_STS_PY_LIB})
else()
    message(WARNING "selfplay-test：找不到 Python 导入库（pyconfig.h 的 pragma 会要它）")
endif()
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

