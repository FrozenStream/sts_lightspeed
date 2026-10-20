#ifndef STS_LIGHTSPEED_SHARDIO_H
#define STS_LIGHTSPEED_SHARDIO_H

#include <cstdint>

#include "selfplay.h" // Frame / Term（一行要写的决策点与终局）
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace sts::collect
{
    // 采集侧的固定宽度（= `azaction.POLICY_DIM` / `SELECT_DIM`；写死在 C++ 侧，`register` 里自检）。
    constexpr int kPolicyDim = 96;
    constexpr int kSelectDim = 128;

    // 容器布局（`collect_io.py` 的 `HDR` / `COLDIR` 同源）：
    // 头 24 = 8(magic)+4(ver)+4(ncols)+8(nrows)；
    // 列目录每项 36 = 16(name)+4(dtype)+8(width)+8(offset)。
    constexpr std::uint64_t kHeaderBytes = 24;
    constexpr std::uint64_t kColDirBytes = 36;
    constexpr std::uint32_t kShardVersion = 5;

    template <typename T>
    inline constexpr const char *shardDtype = nullptr;
    template <>
    inline constexpr const char *shardDtype<float> = "<f4";
    template <>
    inline constexpr const char *shardDtype<double> = "<f8";
    template <>
    inline constexpr const char *shardDtype<std::uint32_t> = "<u4";
    template <>
    inline constexpr const char *shardDtype<std::int32_t> = "<i4";
    template <>
    inline constexpr const char *shardDtype<std::int16_t> = "<i2";
    template <>
    inline constexpr const char *shardDtype<std::uint16_t> = "<u2";
    template <>
    inline constexpr const char *shardDtype<std::uint64_t> = "<u8";
    template <>
    inline constexpr const char *shardDtype<std::int64_t> = "<i8";
    template <>
    inline constexpr const char *shardDtype<std::uint8_t> = "|b1";

    template <typename T, int W>
    struct Buffer
    {
        static constexpr const char *kDtype = shardDtype<T>;
        std::vector<T> data;
        static constexpr int width() { return W; };
        static constexpr int bytewidth() { return W * sizeof(T); };
        void push(const T *v) { data.insert(data.end(), v, v + W); }
        const void *bytes() const { return data.data(); }
        void clearBuffer() { data.clear(); }
    };

    struct ShardBuffers
    {
        Buffer<float, sts::collect::Snap::playerLen> obs_player;
        Buffer<float, sts::collect::Snap::countsLen> obs_counts;
        Buffer<float, sts::collect::Snap::handSlots * sts::collect::Snap::handFields> obs_hand;
        Buffer<float, sts::collect::Snap::pileSlots> obs_draw;
        Buffer<float, sts::collect::Snap::pileSlots> obs_discard;
        Buffer<float, sts::collect::Snap::pileSlots> obs_exhaust;
        Buffer<float, sts::collect::Snap::deckSlots> obs_deck;
        Buffer<float, sts::collect::Snap::monsterSlots * sts::collect::Snap::monsterFields> obs_monster;
        Buffer<float, sts::collect::Snap::playerAttrLen> obs_pattr;
        Buffer<float, sts::collect::Snap::playerCounterLen> obs_pcnt;
        Buffer<float, sts::collect::Snap::playerStatusLen> obs_pstat;
        Buffer<float, sts::collect::Snap::monsterSlots * sts::collect::Snap::monsterStatusLen> obs_mstat;
        Buffer<float, sts::collect::Snap::cardSelectSlots * sts::collect::Snap::cardSelectFields> obs_select;
        Buffer<float, sts::collect::Snap::selectMetaLen> obs_selmeta;
        Buffer<float, sts::collect::Snap::runCtxLen> obs_runctx;
        Buffer<float, sts::collect::Snap::relicLen> obs_relic;
        Buffer<float, sts::collect::Snap::potionMetaLen> obs_pmeta;
        Buffer<float, sts::collect::Snap::potionSlotLen> obs_potion;
        Buffer<std::uint16_t, 96> pi_act;
        Buffer<std::uint16_t, 128> pi_sel;        Buffer<std::uint8_t, 96> m_act;
        Buffer<std::uint8_t, 128> m_sel;
        Buffer<std::int32_t, 1> a_act;
        Buffer<std::int32_t, 1> a_sel;
        Buffer<std::uint8_t, 1> t_win; // `|b1`：它就是 `Term::is_win` 这个 bool（1 字节，同掩码列）
        Buffer<std::int32_t, 1> t_hp;
        Buffer<std::int32_t, 1> t_max_hp;
        Buffer<std::int32_t, 1> t_turn;
        Buffer<std::int32_t, 1> t_enemy_cur_hp;
        Buffer<std::int32_t, 1> t_enemy_max_hp;

        // 列清单**只有这一份**（名字在调用点、类型/宽度/字节都在列对象上）—— KataGo 的
        // `TrainingWriteBuffers::writeToZipFile` 同形：它也是逐列手写（每列两条语句：
        // `prepareHeaderWithNumRows` + `writeBuffer("名字", …)`），名字就是那里的字面量。
        // 顺序 = 文件里的物理顺序（`writeColumns` 两趟、`clear`、`countCols` 全走这里）。
        template <typename F>
        void eachCol(F &&f)
        {
            f("obs_player", obs_player);
            f("obs_counts", obs_counts);
            f("obs_hand", obs_hand);
            f("obs_draw", obs_draw);
            f("obs_discard", obs_discard);
            f("obs_exhaust", obs_exhaust);
            f("obs_deck", obs_deck);
            f("obs_monster", obs_monster);
            f("obs_pattr", obs_pattr);
            f("obs_pcnt", obs_pcnt);
            f("obs_pstat", obs_pstat);
            f("obs_mstat", obs_mstat);
            f("obs_select", obs_select);
            f("obs_selmeta", obs_selmeta);
            f("obs_runctx", obs_runctx);
            f("obs_relic", obs_relic);
            f("obs_pmeta", obs_pmeta);
            f("obs_potion", obs_potion);
            f("pi_act", pi_act);
            f("pi_sel", pi_sel);
            f("m_act", m_act);
            f("m_sel", m_sel);
            f("a_act", a_act);
            f("a_sel", a_sel);
            f("t_win", t_win);
            f("t_hp", t_hp);
            f("t_max_hp", t_max_hp);
            f("t_turn", t_turn);
            f("t_enemy_cur_hp", t_enemy_cur_hp);
            f("t_enemy_max_hp", t_enemy_max_hp);
        }

        /// `const` 语境（`flush` 读列）也走同一份清单 —— `eachCol` **不改任何列**。
        template <typename F>
        void eachCol(F &&f) const
        {
            const_cast<ShardBuffers *>(this)->eachCol([&](const char *nm, auto &c)
                                                      { f(nm, c); });
        }

        void clear()
        {
            eachCol([](const char *, auto &c)
                    { c.clearBuffer(); });
        }

        // 写**一行**：把 `Frame`（一个决策点）+ `Term`（那一场的终局读数）填进各列。
        // **写动作属于"一组列"**（KataGo 的 `TrainingWriteBuffers::addRow` 同形）。
        // ⚠️ 只装**训练要用**的东西：观测 / 策略目标与掩码 / 实际动作与行为概率 / 终局列。
        //    场身份（`seed`/`enc`/`enc_floor`）也都不落盘（训练不看它们；段号由终局列现推）。
        // ⚠️ 局部量加 `v_` 前缀：`obs` 这些**同时也是列成员名**。
        inline void add(const Frame &fr, const Term &term)
        {
            const float *v_obs = fr.obs.data();
            const auto &pi_idx = fr.pi_idx;
            const auto &pi_n = fr.pi_n;
            const auto &legal = fr.legal;
            const bool &is_sel = fr.is_sel;
            const int &a_idx = fr.a_idx;

            // 两个头各是**定长数组**（栈上、零初始化），只填本空间有效的那一个。
            // v4 存**未归一访问数**（u16）；训练读侧再归一化。省一半列宽，且保留原始计数。
            std::array<std::uint16_t, kPolicyDim> pa{};
            std::array<std::uint16_t, kSelectDim> ps{};
            const auto count16 = [](std::int64_t n)
            {
                return static_cast<std::uint16_t>(std::clamp<std::int64_t>(n, 0, 65535));
            };
            if (is_sel)
            {
                for (std::size_t i = 0; i < pi_idx.size(); ++i)
                {
                    const int at = pi_idx[i];
                    if (at >= 0 && at < kSelectDim)
                    {
                        ps[static_cast<std::size_t>(at)] = count16(pi_n[i]);
                    }
                }
            }
            else
            {
                for (std::size_t i = 0; i < pi_idx.size(); ++i)
                {
                    const int at = pi_idx[i];
                    if (at >= 0 && at < kPolicyDim)
                    {
                        pa[static_cast<std::size_t>(at)] = count16(pi_n[i]);
                    }
                }
            }

            // 掩码：**每个下标 1 字节**（0/1）—— 与 Python `assemble` 的 `bool` 列同口径。
            std::array<std::uint8_t, kPolicyDim> ma{};
            std::array<std::uint8_t, kSelectDim> ms{};
            for (int i : legal)
            {
                if (is_sel)
                {
                    if (i >= 0 && i < kSelectDim)
                    {
                        ms[static_cast<std::size_t>(i)] = 1;
                    }
                }
                else
                {
                    if (i >= 0 && i < kPolicyDim)
                    {
                        ma[static_cast<std::size_t>(i)] = 1;
                    }
                }
            }

            // 动作下标：非本空间写 −1（同 `assemble`）。
            const std::int32_t aa = is_sel ? -1 : a_idx;
            const std::int32_t as = is_sel ? a_idx : -1;
            const std::uint8_t v_win = term.is_win ? 1 : 0; // 列是 `|b1`，`bool` 得先落到 1 字节

            // 观测落盘：写侧已经是归一化 float；这里只按块切列，读侧直接拼回。
            const auto pushBlock = [&](auto &col, int off)
            { col.push(v_obs + off); };
            pushBlock(obs_player, sts::collect::Snap::playerOff);
            pushBlock(obs_counts, sts::collect::Snap::countsOff);
            pushBlock(obs_hand, sts::collect::Snap::handOff);
            pushBlock(obs_draw, sts::collect::Snap::drawOff);
            pushBlock(obs_discard, sts::collect::Snap::discardOff);
            pushBlock(obs_exhaust, sts::collect::Snap::exhaustOff);
            pushBlock(obs_deck, sts::collect::Snap::deckOff);
            pushBlock(obs_monster, sts::collect::Snap::monsterOff);
            pushBlock(obs_pattr, sts::collect::Snap::playerAttrOff);
            pushBlock(obs_pcnt, sts::collect::Snap::playerCounterOff);
            pushBlock(obs_pstat, sts::collect::Snap::playerStatusOff);
            pushBlock(obs_mstat, sts::collect::Snap::monsterStatusOff);
            pushBlock(obs_select, sts::collect::Snap::cardSelectOff);
            pushBlock(obs_selmeta, sts::collect::Snap::selectMetaOff);
            pushBlock(obs_runctx, sts::collect::Snap::runCtxOff);
            pushBlock(obs_relic, sts::collect::Snap::relicOff);
            pushBlock(obs_pmeta, sts::collect::Snap::potionOff);
            pushBlock(obs_potion, sts::collect::Snap::potionSlotOff);
            pi_act.push(pa.data());
            pi_sel.push(ps.data());
            m_act.push(ma.data());
            m_sel.push(ms.data());
            a_act.push(&aa);
            a_sel.push(&as);
            t_win.push(&v_win);
            t_hp.push(&term.cur_hp);
            t_max_hp.push(&term.max_hp);
            t_turn.push(&term.turn);
            t_enemy_cur_hp.push(&term.enemy_cur_hp);
            t_enemy_max_hp.push(&term.enemy_max_hp);
        }

        // **列集合自己给字节**（不用在 `flush` 里造一个 `BufferRef` 数组）：
        //   名字/类型/宽度/行字节数都是成员自带的 ⇒ 列数不必手抄常量，数一遍即可。
        int countCols() const
        {
            int n = 0;
            eachCol([&](const char *, const auto &)
                    { ++n; });
            return n;
        }

        // 写列目录 + 数据（`nrows` 行）。数据段起点 = 头 + 列目录，**自己算**（调用方不用知道 56/36）。
        // 第 5 个实参是**行字节数**（`bytewidth()`）⇒ 乘 `nrows` 才是该列的总字节数。
        template <typename Out>
        void writeColumns(Out &out, std::uint64_t nrows) const
        {
            const std::uint64_t data_at =
                kHeaderBytes + kColDirBytes * static_cast<std::uint64_t>(countCols());
            // 第一趟：写列目录（偏移现算，**不存数组**）
            std::uint64_t cur = data_at;
            eachCol([&](const char *nm, const auto &c)
                    {
                        char name16[16] = {0};
                        std::memcpy(name16, nm, std::min<std::size_t>(std::strlen(nm), 15));
                        out.write(name16, 16);
                        char dtb[4] = {0}; // 目录里固定 4 字节（`<f4` + NUL 就占满）
                        std::memcpy(dtb, c.kDtype, std::min<std::size_t>(std::strlen(c.kDtype), 3));
                        out.write(dtb, 4);
                        const std::uint64_t ww = static_cast<std::uint64_t>(c.width());
                        out.write(reinterpret_cast<const char *>(&ww), 8);
                        out.write(reinterpret_cast<const char *>(&cur), 8);
                        cur += c.bytewidth() * nrows; });
            // 第二趟：写数据（同上顺序）
            eachCol([&](const char *nm, const auto &c)
                    {
                        (void)nm;
                        const std::uint64_t nb = c.bytewidth() * nrows;
                        if (nb > 0) { out.write(static_cast<const char *>(c.bytes()), (std::streamsize)nb); } });
        }
    };

    // 分片写入器：攒够行就 `flush()` 写一个文件（切分点由调用方按 `shard_rows` 管）。
    class ShardWriter
    {
    public:
        std::string path; // 输出文件路径（本容器；扩展名随意）
        int n = 0;        // 已攒行数
        ShardBuffers b;

        ShardWriter() = default;

        void endRow() { ++n; }

        int size() const { return n; }

        inline std::string flush()
        {
            // ⚠️ 没攒到行就**返回空串**（调用方靠 `empty()` 判断"这次没产出文件"）——
            //    曾经返回 `"Nothing write"`（非空！）⇒ 空 flush 会被当成产出路径收进列表。
            if (n <= 0)
                return std::string();

            // 列数**数一遍**（`eachCol` 是唯一真相 ⇒ 不手抄常量）；数据段起点在 `writeColumns` 里算。
            const std::uint32_t ncols = static_cast<std::uint32_t>(b.countCols());
            const std::uint64_t nrows = static_cast<std::uint64_t>(n);

            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                throw std::runtime_error("ShardWriter：打不开 " + path);
            }
            const char magic[8] = {'S', 'T', 'S', 'S', 'H', 'R', 'D', '1'};
            out.write(magic, 8);
            out.write(reinterpret_cast<const char *>(&kShardVersion), 4);
            out.write(reinterpret_cast<const char *>(&ncols), 4);
            out.write(reinterpret_cast<const char *>(&nrows), 8);

            b.writeColumns(out, nrows);

            out.close();
            if (!out)
            {
                throw std::runtime_error("ShardWriter：写 " + path + " 失败");
            }

            const std::string written = path;
            n = 0;
            b.clear();
            return written;
        }

        void add(const Frame &fr, const Term &term) { b.add(fr, term); }
    };

} // namespace sts::collect

#endif // STS_LIGHTSPEED_SHARDIO_H