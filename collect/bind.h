//
// 绑定注册点的**声明**（与实现分开住：实现要 pybind11，声明不要）。
//
// ⚠️ 为什么单独一个头：`collect/*.h` 是**非绑定消费者**（`apps/*-test.cpp`）也会 include 的，
//    而 pybind11 的头会拉进 `Python.h`（还有 `pyconfig.h` 自带的
//    `#pragma comment(lib,"python3XX.lib")`）⇒ 链接期逼着那个可执行文件依赖 `python3XX.dll`
//    （实测：ORT 后端下 `selfplay-test.exe` 就只剩这一个 DLL 依赖）。把 `registerXxx` 的声明挪出来，
//    热循环 / 测试那一侧就彻底不碰 Python。
//
#ifndef STS_LIGHTSPEED_COLLECT_BIND_H
#define STS_LIGHTSPEED_COLLECT_BIND_H

#include <pybind11/pybind11.h>

namespace sts::collect
{
    void registerSelfplay(pybind11::module_ &m);
    void registerEval(pybind11::module_ &m);
    void registerTeacher(pybind11::module_ &m);
} // namespace sts::collect

#endif // STS_LIGHTSPEED_COLLECT_BIND_H
