//
// `teacher` 的**绑定层**（与 `runTeacherAll` 的调度分开住：那一侧不碰 pybind11）。
//
// ⚠️ 为什么单独一个 TU：`collect/teacher.cpp` 现在也被**非绑定消费者**编（`apps/selfplay-failure-test`，
//    为了让它的编译告警有落点）。pybind11 的头会拉 `Python.h`，而 `pyconfig.h` 自带
//    `#pragma comment(lib,"python3XX.lib")` ⇒ 那个 TU 里只要出现过 pybind11，链接期就会让可执行文件
//    硬依赖 `python3XX.dll`。同形的先例：`collect/selfplay_bind.cpp` / `collect/scenebuild_bind.cpp`。
//
#include "teacher.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h> // `teacher_run` 返回 vector<string>：转换器要在这个 TU 里可见

namespace sts::collect
{
    void registerTeacher(pybind11::module_ &m)
    {
        namespace py = pybind11;
        py::class_<TeacherConfig>(m, "TeacherConfig")
            .def(py::init<>())
            .def_readwrite("sims", &TeacherConfig::sims)
            .def_readwrite("max_steps", &TeacherConfig::max_steps)
            .def_readwrite("tau", &TeacherConfig::tau)
            .def_readwrite("eps", &TeacherConfig::eps)
            .def_readwrite("threads", &TeacherConfig::threads);
        m.def(
            "teacher_run",
            [](int n_per, const TeacherConfig &cfg, std::uint64_t start_seed, const std::string &out_dir, int shard_rows)
            {
                return sts::collect::teacherRun(n_per, cfg, start_seed, out_dir, shard_rows);
            },
            py::arg("n_per"), py::arg("cfg"), py::arg("start_seed"), py::arg("out_dir"), py::arg("shard_rows") = 200000,
            "**整段采集**（任务表 + 造局面 + 搜索 + 落盘全在 C++，不经过 Python）：\n"
            "按**引擎的遭遇总表**（`MonsterEncounterPool::allEnemies`）铺开 `n_per` 场/项，分片写进\n"
            "`out_dir`，返回路径列表。每行自带 `enc` / `enc_floor` 两列 ⇒ 调用方不必知道表长什么样。");
        m.def(
            "teacher_turn_kill",
            [](const BattleContext &bc, int target, const TeacherConfig &cfg, std::uint64_t seed, int max_actions)
            {
                return sts::collect::teacherTurnKills(bc, target, cfg, seed, max_actions);
            },
            py::arg("bc"), py::arg("target"), py::arg("cfg"), py::arg("seed"),
            py::arg("max_actions") = 50,
            "从给定局面只打当前回合；返回 target 是否在回合结束前死亡。");
    }

} // namespace sts::collect