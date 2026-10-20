//
// `collect/scenebuild.cpp` 的 **pybind 层** —— 故意与引擎逻辑分开成两个 TU。
//
// ⚠️ 为什么必须分：`scenebuild.cpp` 是纯 C++（造局面），而它原来把 `registerSceneBuild`
//    也放在同一个 TU 里 ⇒ 任何想链接它的 **C++ 测试/工具**都会拖进 pybind11 的符号
//    （`PyObject_*`），非模块目标是编不过/链不上的。生产侧不受影响（`slaythespire` 两个
//    TU 都编），但 `apps/scene-test.cpp` 这类必须能单独编。
//    照 KataGo 的分法：引擎逻辑在 `game/` / `search/`，`command/`+`main.cpp` 才是外壳。
//
#include "scenebuild_bind.h"

#include "scenebuild.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>

namespace sts::collect
{

    void registerSceneBuild(pybind11::module_ &m)
    {
        namespace py = pybind11;

        m.def("build_battle", &buildBattle, py::arg("seed"), py::arg("enc"), py::arg("floor"), py::arg("ascension"),
              "按层造一局战斗（本文件的 `shapeFloor`；同 seed 逐位可复现）");
        m.def("ascension_of", &ascensionOf, py::arg("seed"), "按 seed 抽本场的进阶等级（唯一决定 A10+ 的进阶之灾）");
        m.def("act_of", &actOf, py::arg("floor"), "层号 → 幕（17/34/51 是幕分界；唯一的实现，`scene.act_of` 只是转发）");
    }

} // namespace sts::collect
