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
            .def_readwrite("sims_cheap", &SelfplayConfig::sims_cheap)
            .def_readwrite("full_frac", &SelfplayConfig::full_frac)
            .def_readwrite("max_steps", &SelfplayConfig::max_steps)
            .def_readwrite("threads", &SelfplayConfig::threads)
            .def_readwrite("pool", &SelfplayConfig::pool)
            .def_readwrite("workers", &SelfplayConfig::workers)
            .def_readwrite("batch_target", &SelfplayConfig::batch_target)
            .def_readwrite("batch_wait_us", &SelfplayConfig::batch_wait_us)
            .def_readwrite("c_puct", &SelfplayConfig::c_puct)
            .def_readwrite("q_init", &SelfplayConfig::q_init)
            .def_readwrite("sample", &SelfplayConfig::sample)
            .def_readwrite("eps", &SelfplayConfig::eps)
            .def_readwrite("temp_moves", &SelfplayConfig::temp_moves)
            .def_readwrite("dirichlet_total", &SelfplayConfig::dirichlet_total)
            .def_readwrite("dirichlet_eps", &SelfplayConfig::dirichlet_eps)
            // 尾段（窄批）分流：空路径 + 0 = 关（原路径逐位不变）。
            .def_readwrite("model_path_cpu", &SelfplayConfig::model_path_cpu)
            .def_readwrite("cpu_sessions", &SelfplayConfig::cpu_sessions)
            .def_readwrite("cpu_rows", &SelfplayConfig::cpu_rows);

        m.def(
            "selfplay_run",
            [](int n_per, const SelfplayConfig &cfg, std::uint64_t start_seed,
               const std::string &out_dir, int shard_rows)
            {
                // 整段不碰 Python ⇒ 放 GIL（与 `teacher_run` 同）。
                py::gil_scoped_release release;
                return sts::collect::selfplayRun(n_per, cfg, start_seed, out_dir, shard_rows);
            },
            py::arg("n_per"), py::arg("cfg"), py::arg("start_seed"), py::arg("out_dir"),
            py::arg("shard_rows") = 200000,
            "**整段采集**（任务表 + 造局面 + 搜索 + 落盘全在 C++，不经过 Python）：每项 `n_per` 场，\n"
            "`cfg.pool` = 池宽（在飞的场数 = 前向批宽上限，跑完立刻补新场），`cfg.workers` = worker 线程数。\n"
            "分片写进 `out_dir`，返回路径列表。");
    }

} // namespace sts::collect
