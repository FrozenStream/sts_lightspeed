//
// `scenebuild.cpp`（**纯 C++**：按层造发育状态）的 pybind 入口声明。
//
// ⚠️ 声明放这里而不是 `scenebuild.h`：那个头要给 `apps/scene-test.cpp` 这类**非模块**目标用，
//    带上 `<pybind11/...>` 就得给每个目标都加 pybind11 的 include 目录（而它们根本不需要）。
//    实现见 `scenebuild_bind.cpp`。
//
#ifndef STS_LIGHTSPEED_SCENEBUILD_BIND_H
#define STS_LIGHTSPEED_SCENEBUILD_BIND_H

#include <pybind11/pybind11.h>

namespace sts::collect
{

void registerSceneBuild(pybind11::module_ &m);

} // namespace sts::collect

#endif //STS_LIGHTSPEED_SCENEBUILD_BIND_H
