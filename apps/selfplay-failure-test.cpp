// 使用可控的推理器验证异步异常传播、线程收尾与落盘外壳（`ShardSink`），不依赖外部模型。
#include "selfplay.h"
#include "teacher.h"
#include "encs.h"
#include "selfplay_forward.h"
#include "scenebuild.h"
#include "onnx_eval.h"
#include "shard_out.h"
#include "shardio.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace sts::py
{
    struct OnnxEvaluator::Impl
    {
        std::string fault;
        int calls = 0;
        std::vector<float> pa, ps, val;
    };

    OnnxEvaluator::OnnxEvaluator(const std::string &path, const std::string &, int, bool)
        : impl_(std::make_unique<Impl>())
    {
        if (path == "load-failure")
        {
            throw std::runtime_error("injected load failure");
        }
        impl_->fault = path;
        p_act_dim = collect::kPolicyDim;
        p_sel_dim = collect::kSelectDim - (path == "wrong-width" ? 1 : 0);
    }

    OnnxEvaluator::~OnnxEvaluator() = default;

    OnnxOut OnnxEvaluator::eval(const float *, std::int64_t n, std::int64_t)
    {
        if (++impl_->calls > 1 && impl_->fault == "inference-failure")
        {
            throw std::runtime_error("injected inference failure");
        }
        const float policy = impl_->fault == "nan-policy" ? std::numeric_limits<float>::quiet_NaN() : 0.0f;
        impl_->pa.assign(static_cast<std::size_t>(n) * p_act_dim, policy);
        impl_->ps.assign(static_cast<std::size_t>(n) * p_sel_dim, policy);
        impl_->val.assign(static_cast<std::size_t>(n), 0.0f);
        return {impl_->pa.data(), impl_->ps.data(), impl_->val.data()};
    }
}

namespace
{
    void require(bool condition, const char *message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    template <typename F> void expectError(const char *text, F &&run)
    {
        try
        {
            run();
        }
        catch (const std::exception &e)
        {
            require(std::string(e.what()).find(text) != std::string::npos, e.what());
            std::fprintf(stderr, "  pass: %s\n", text);
            return;
        }
        throw std::runtime_error(std::string("missing error: ") + text);
    }

    void checkFailures()
    {
        using namespace sts;
        using namespace sts::collect;
        SelfplayConfig cfg;
        cfg.model_path = "normal";
        cfg.device = "cpu";
        cfg.z_spec = "hp";
        cfg.sims = 2;
        cfg.sims_cheap_frac = 0.0; // 失败路径不掺随机化预算（只跑精确 sims）
        cfg.max_steps = 1;
        cfg.workers = 2;
        cfg.c_puct = 1.5;
        const auto battle = buildBattle(0, MonsterEncounter::CULTIST, 4, ascensionOf(0));
        const auto make = [&](std::size_t) { return battle; };
        const auto discard = [](std::size_t, GameOut &&) {};

        expectError("injected initial failure", [&] {
            selfplayPooled(
                2, 2,
                [&](std::size_t j) {
                    if (j == 1)
                        throw std::runtime_error("injected initial failure");
                    return make(j);
                },
                cfg, discard);
        });
        expectError("injected refill failure", [&] {
            selfplayPooled(
                4, 1,
                [&](std::size_t j) {
                    if (j == 1)
                        throw std::runtime_error("injected refill failure");
                    return make(j);
                },
                cfg, discard);
        });
        expectError("injected delivery failure", [&] {
            selfplayPooled(4, 2, make, cfg, [](std::size_t, GameOut &&) {
                throw std::runtime_error("injected delivery failure");
            });
        });
        cfg.model_path = "inference-failure";
        expectError("injected inference failure", [&] { selfplayPooled(4, 2, make, cfg, discard); });
        cfg.model_path = "nan-policy";
        expectError("非有限策略", [&] { selfplayPooled(4, 2, make, cfg, discard); });
        cfg.model_path = "normal";
        cfg.model_path_cpu.clear();

        cfg.model_path = "wrong-width";
        expectError("动作空间不一致", [&] { selfplayPooled(1, 1, make, cfg, discard); });
        cfg.model_path = "normal";
        expectError("已结束的初始战斗", [&] {
            auto ended = battle;
            ended.outcome = Outcome::PLAYER_VICTORY;
            selfplayPooled(1, 1, [&](std::size_t) { return ended; }, cfg, discard);
        });

        expectError("n_per", [&] { selfplayRun(0, cfg, 0, ".", 1); });
        expectError("shard_rows", [&] { selfplayRun(1, cfg, 0, ".", 0); });
        cfg.dirichlet_eps = 0.25;
        expectError("dirichlet_total", [&] { selfplayPooled(1, 1, make, cfg, discard); });
        cfg.dirichlet_eps = 0.0;

        // 每个任务只构造一次；环满恢复后仍按序交付。
        std::vector<int> created(40);
        std::mutex created_lock;
        std::size_t delivered = 0;
        selfplayPooled(
            created.size(), 3,
            [&](std::size_t j) {
                std::lock_guard<std::mutex> lock(created_lock);
                require(++created.at(j) == 1, "task constructed twice");
                return make(j);
            },
            cfg,
            [&](std::size_t j, GameOut &&g) {
                require(j == delivered++, "delivery order changed");
                require(g.frames.size() == 1, "wrong frame count");
                if (j == 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            });
        require(delivered == created.size(), "tasks lost");
        for (int count : created)
            require(count == 1, "task not constructed");

        // 尾段（worker 就地前向）开着：接手之后仍须每场恰好交付一次、按序交付。
        cfg.model_path_cpu = "normal";
        delivered = 0;
        selfplayPooled(32, 4, make, cfg, [&](std::size_t j, GameOut &&g) {
            require(j == delivered++, "mixed inference delivery order changed");
            require(g.frames.size() == 1, "mixed inference lost a frame");
        });
        require(delivered == 32, "mixed inference lost tasks");

        // 不同池宽和 worker 数反复跨过"在飞场数 ≤ worker"（尾段接管）的边界，
        // 所有任务必须恰好交付一次。
        cfg.max_steps = 3;
        cfg.sims = 4;
        for (int round = 0; round < 24; ++round)
        {
            cfg.workers = 1 + round % 4;
            delivered = 0;
            selfplayPooled(17, static_cast<std::size_t>(2 + round % 6), make, cfg,
                           [&](std::size_t j, GameOut &&g) {
                               require(j == delivered++, "split queue delivery order changed");
                               require(!g.frames.empty(), "split queue lost a game");
                           });
            require(delivered == 17, "split queue lost tasks");
        }

        cfg.model_path = "load-failure";
        selfplayPooled(0, 1, make, cfg, discard);
    }

    // 分片头：`STSSHRD1` + version u32 + ncols u32 + nrows u64（同 `shardio.h` 的 `writeColumns`）。
    std::uint64_t shardRows(const std::string &path)
    {
        using sts::collect::kShardVersion;

        std::ifstream in(path, std::ios::binary);
        require(in.good(), "打不开分片");
        char magic[8] = {0};
        std::uint32_t version = 0, ncols = 0;
        std::uint64_t nrows = 0;
        in.read(magic, 8);
        in.read(reinterpret_cast<char *>(&version), 4);
        in.read(reinterpret_cast<char *>(&ncols), 4);
        in.read(reinterpret_cast<char *>(&nrows), 8);
        require(std::string(magic, 8) == "STSSHRD1", "分片 magic 不对");
        require(version == kShardVersion, "分片版本不对");
        require(ncols > 0, "分片没有列");
        return nrows;
    }

    void checkTeacherScheduling()
    {
        using namespace sts::collect;
        namespace fs = std::filesystem;
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path dir = fs::temp_directory_path() / ("sts_teacher_test_" + std::to_string(nonce));
        struct Cleanup
        {
            fs::path path;
            ~Cleanup()
            {
                std::error_code ignored;
                fs::remove_all(path, ignored);
            }
        } cleanup{dir};

        TeacherConfig cfg;
        // 不跑搜索 playout，避免搜索器的全局调试计数干扰调度测试。
        cfg.sims = 0;
        cfg.eps = 1.0;
        cfg.max_steps = 1;
        cfg.threads = 1;
        const auto serial = teacherRun(2, cfg, 0, (dir / "serial").string(), 10000);
        const auto expected_rows = encJobs(2).size();
        require(serial.size() == 1 && shardRows(serial[0]) == expected_rows,
                "teacher dropped or duplicated games");
        const auto bytes = [](const std::string &path)
        {
            std::ifstream in(path, std::ios::binary);
            require(in.good(), "cannot open teacher shard");
            return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        };
        const auto expected = bytes(serial[0]);
        for (int workers : {2, 4})
        {
            cfg.threads = workers;
            const auto parallel = teacherRun(2, cfg, 0, (dir / std::to_string(workers)).string(), 10000);
            require(parallel.size() == 1 && bytes(parallel[0]) == expected,
                    "teacher scheduling changed shard contents or game order");
        }

        const auto blocked = dir / "blocked";
        fs::create_directories(blocked / "blk00000.bin");
        expectError("ShardWriter", [&] {
            teacherRun(2, cfg, 0, blocked.string(), 1);
        });

        cfg.sims = 0;
        cfg.eps = 0.0;
        expectError("教师搜不出", [&] {
            teacherRun(2, cfg, 0, (dir / "worker-failure").string(), 1);
        });

        cfg.eps = 1.0;
        const auto recovered = teacherRun(2, cfg, 0, (dir / "recovered").string(), 10000);
        require(recovered.size() == 1 && bytes(recovered[0]) == expected,
                "teacher failed to recover after delivery or worker failure");
        std::fputs("  pass: teacher ordered delivery and failure cleanup\n", stderr);
    }

    // 按**列目录**取一列的 i4 值（名字/类型/宽度/偏移都在目录里 ⇒ 不手抄布局）。
    std::vector<std::int32_t> shardI4Column(const std::string &path, const char *want)
    {
        std::ifstream in(path, std::ios::binary);
        require(in.good(), "打不开分片");
        char magic[8] = {0};
        std::uint32_t version = 0, ncols = 0;
        std::uint64_t nrows = 0;
        in.read(magic, 8);
        in.read(reinterpret_cast<char *>(&version), 4);
        in.read(reinterpret_cast<char *>(&ncols), 4);
        in.read(reinterpret_cast<char *>(&nrows), 8);
        require(std::string(magic, 8) == "STSSHRD1", "分片 magic 不对");
        for (std::uint32_t i = 0; i < ncols; ++i)
        {
            char name[16] = {0}, dtype[4] = {0};
            std::uint64_t width = 0, offset = 0;
            in.read(name, 16);
            in.read(dtype, 4);
            in.read(reinterpret_cast<char *>(&width), 8);
            in.read(reinterpret_cast<char *>(&offset), 8);
            if (std::strcmp(name, want) != 0)
            {
                continue;
            }
            require(width == 1 && std::strcmp(dtype, "<i4") == 0, "该列不是 i4×1");
            std::vector<std::int32_t> out(static_cast<std::size_t>(nrows));
            in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(nrows) * 4);
            return out;
        }
        throw std::runtime_error(std::string("分片里没有列 ") + want);
    }

    // 落盘外壳（`collect/shard_out.h`）三件事一起守：分片名**补零**（读侧按名字序拼行序 ⇒
    // `blk2` 必须排在 `blk10` 前面）、切分点**按行**（同一场的行连续、可以切在场中间）、
    // 空产物**不返回路径**。
    void checkShardSink()
    {
        using sts::Outcome;
        using sts::collect::Frame;
        using sts::collect::GameOut;
        using sts::collect::ShardSink;

        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "sts_shard_sink_test";
        fs::remove_all(dir);

        const auto frame = [](int a_idx)
        {
            Frame f{};
            f.pi_idx = {a_idx};
            f.pi_n = {3};
            f.legal = {a_idx};
            f.a_idx = a_idx;
            return f;
        };
        // 一场 `n` 帧、`a_act` 从 `first` 递增 ⇒ 按列读回来就能验"行序 == 场序"。
        const auto game = [&](int first, int n)
        {
            GameOut g;
            for (int i = 0; i < n; ++i)
            {
                g.frames.push_back(frame(first + i));
            }
            g.term.turn = n;
            g.outcome = Outcome::PLAYER_VICTORY;
            return g;
        };

        // 空产物：一场都没写 ⇒ `finish()` 必须是空列表（`flush()` 没攒到行时返回空串）。
        {
            ShardSink sink(dir.string(), 1000);
            require(sink.finish().empty(), "空产物返回了路径");
        }

        // `shard_rows = 1` ⇒ 一帧一个分片；编号补零 ⇒ 名字序 == 写序。
        {
            ShardSink sink(dir.string(), 1);
            sink.addGame(game(0, 12));
            const std::vector<std::string> got = sink.finish();
            require(got.size() == 12, "shard_rows=1 没有按行切分");
            std::vector<std::string> sorted = got;
            std::sort(sorted.begin(), sorted.end());
            require(sorted == got, "分片名序与写序不一致（少了补零）");
            require(fs::path(got[10]).filename().string() == "blk00010.bin", "分片命名变了");
            for (const std::string &p : got)
            {
                require(shardRows(p) == 1, "分片的行数不是 1");
            }
        }

        fs::remove_all(dir);

        // 6 帧写两个分片（`shard_rows = 4`）：切分**按行**（切在场中间），行序 == 场序。
        {
            ShardSink sink(dir.string(), 4);
            sink.addGame(game(0, 3));
            sink.addGame(game(3, 3));
            const std::vector<std::string> got = sink.finish();
            require(got.size() == 2, "切分点数不对");
            const std::vector<std::int32_t> a0 = shardI4Column(got[0], "a_act");
            const std::vector<std::int32_t> a1 = shardI4Column(got[1], "a_act");
            require((a0 == std::vector<std::int32_t>{0, 1, 2, 3}), "第一个分片的行序不对");
            require((a1 == std::vector<std::int32_t>{4, 5}), "第二个分片的行序不对");
        }

        fs::remove_all(dir);
        std::fputs("  pass: 落盘外壳（命名 / 切分 / 空产物）\n", stderr);
    }
}

int main()
{
    std::mutex lock;
    std::condition_variable cv;
    bool finished = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> guard(lock);
        if (!cv.wait_for(guard, std::chrono::seconds(20), [&] { return finished; }))
        {
            std::fputs("selfplay failure tests timed out\n", stderr);
            std::fflush(stderr);
            std::_Exit(2);
        }
    });
    int result = 0;
    try
    {
        checkFailures();
        checkTeacherScheduling();
        checkShardSink();
        std::fputs("selfplay failure tests passed\n", stderr);
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "selfplay failure tests failed: %s\n", e.what());
        result = 1;
    }
    {
        std::lock_guard<std::mutex> guard(lock);
        finished = true;
    }
    cv.notify_all();
    watchdog.join();
    return result;
}
