//
// `selfplay` 的**绑定层**（与热循环分开住：热循环不碰 pybind11）。
//
// ⚠️ 为什么单独一个 TU：`collect/selfplay.cpp` 是**非绑定消费者**（`apps/selfplay-test.cpp`、
//    以后的引擎可执行文件）也会编的。pybind11 的头会拉 `Python.h`，而 `pyconfig.h` 自带
//    `#pragma comment(lib,"python3XX.lib")` ⇒ 只要那个 TU 里出现过 pybind11，**链接期**就会让
//    可执行文件硬依赖 `python3XX.dll`（实测：ORT 后端下 `selfplay-test.exe` 就只剩这一个依赖）。
//    把绑定拆出来 ⇒ 测试/引擎那一侧彻底不碰 Python（同形的先例：`collect/scenebuild_bind.cpp`）。
//
#include "selfplay.h"
#include "selfplay_tree.h"
#include "net_eval.h"
#include "snapshot.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace sts::collect
{
    void registerSelfplay(pybind11::module_ &m)
    {
        namespace py = pybind11;

        py::class_<SelfplayConfig>(m, "SelfplayConfig")
            .def(py::init<>())
            .def_readwrite("model_path", &SelfplayConfig::model_path)
            .def_readwrite("z_spec", &SelfplayConfig::z_spec)
            .def_readwrite("device", &SelfplayConfig::device)
            .def_readwrite("sims", &SelfplayConfig::sims)
            .def_readwrite("sims_cheap_frac", &SelfplayConfig::sims_cheap_frac)
            .def_readwrite("full_frac", &SelfplayConfig::full_frac)
            .def_readwrite("max_steps", &SelfplayConfig::max_steps)
            .def_readwrite("pool", &SelfplayConfig::pool)
            .def_readwrite("workers", &SelfplayConfig::workers)
            .def_readwrite("long_decay_start", &SelfplayConfig::long_decay_start)
            .def_readwrite("c_puct", &SelfplayConfig::c_puct)
            .def_readwrite("q_init", &SelfplayConfig::q_init)
            .def_readwrite("sample", &SelfplayConfig::sample)
            .def_readwrite("eps", &SelfplayConfig::eps)
            .def_readwrite("temp_moves", &SelfplayConfig::temp_moves)
            .def_readwrite("dirichlet_total", &SelfplayConfig::dirichlet_total)
            .def_readwrite("dirichlet_eps", &SelfplayConfig::dirichlet_eps)
            // 尾段（worker 就地前向）用的 CPU 存档；空 = 用主存档 + CPU。
            .def_readwrite("model_path_cpu", &SelfplayConfig::model_path_cpu);

        m.def(
            "selfplay_run",
            [](int n_per, const SelfplayConfig &cfg, std::uint64_t start_seed,
               const std::string &out_dir, int shard_rows, bool hard_only)
            {
                // 整段不碰 Python ⇒ 放 GIL（与 `teacher_run` 同）。
                py::gil_scoped_release release;
                return sts::collect::selfplayRun(n_per, cfg, start_seed, out_dir, shard_rows, hard_only);
            },
            py::arg("n_per"), py::arg("cfg"), py::arg("start_seed"), py::arg("out_dir"),
            py::arg("shard_rows") = 200000, py::arg("hard_only") = false,
            "**整段采集**（任务表 + 造局面 + 搜索 + 落盘全在 C++，不经过 Python）：每项 `n_per` 场，\n"
            "`cfg.pool` = 池宽（在飞的场数 = 前向批宽上限，跑完立刻补新场），`cfg.workers` = worker 线程数。\n"
            "分片写进 `out_dir`，返回路径列表。");

        m.def(
            "net_turn_kill",
            [](const BattleContext &start, int target, const SelfplayConfig &cfg, int max_actions)
            {
                sts::py::NetEvaluator eval(cfg.model_path, cfg.device, 1);
                sts::collect::detail::Tree tree(0, start);
                const int turn0 = tree.real.turn;
                for (int step = 0; step < max_actions && tree.real.outcome == Outcome::UNDECIDED; ++step)
                {
                    if (target >= 0 && target < tree.real.monsters.monsterCount &&
                        tree.real.monsters.arr[target].curHp <= 0)
                    {
                        return true;
                    }
                    sts::collect::detail::resetTree(tree, cfg);
                    for (int s = 0; s < cfg.sims; ++s)
                    {
                        if (sts::collect::detail::descendOne(tree, cfg))
                        {
                            continue;
                        }
                        tree.replay();
                        const auto obs = sts::collect::snapshotOf(tree.scratch);
                        const auto out = eval.eval(obs.data(), 1, obs.size());
                        sts::collect::detail::expandBackup(tree, cfg, out.pa, *out.val);
                    }
                    Frame f = sts::collect::detail::recordFrame(tree);
                    sts::collect::detail::stepAndAdvance(tree, cfg, f);
                    if (tree.real.turn != turn0)
                    {
                        break;
                    }
                }
                return target >= 0 && target < tree.real.monsters.monsterCount &&
                       tree.real.monsters.arr[target].curHp <= 0;
            },
            py::arg("bc"), py::arg("target"), py::arg("cfg"), py::arg("max_actions") = 50,
            "从给定局面只打当前回合（网络 MCTS）；返回 target 是否在回合结束前死亡。");
    }

} // namespace sts::collect
