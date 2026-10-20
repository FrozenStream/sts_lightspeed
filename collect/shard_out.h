// 一次采集的**落盘外壳**：目录 + 分片命名 + 按场切分 + 路径汇总。
//
// 两个采集入口共用（`selfplayRun` / `teacherRun`）。契约：**写侧按场序排空** ⇒ 行序 == 场序，
// 同一场的行连续（读侧靠"终局列连续相同的一段 = 一场"认场，落盘里没有场身份列）。
#ifndef STS_LIGHTSPEED_SHARD_OUT_H
#define STS_LIGHTSPEED_SHARD_OUT_H

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "collect.h" // GameOut
#include "shardio.h" // ShardWriter

namespace sts::collect
{
    // ⚠️ `shard_rows` 的校验与目录创建都在这里做，各只做一次：两个采集入口不再各写一份
    //    （`shard_rows = 0` 时 `size() >= 0` 恒真 ⇒ 曾经静默退化成"一行一个文件"）。
    class ShardSink
    {
    public:
        ShardSink(const std::string &out_dir, int shard_rows) : shard_rows_(shard_rows)
        {
            if (shard_rows_ <= 0)
            {
                throw std::runtime_error("shard_rows 必须为正数");
            }
            std::filesystem::create_directories(out_dir);
            dir_ = out_dir;
            setPath();
        }

        // 写一场（`frames` 为空 = 本场没有可记的决策点 ⇒ 只丢这一场，不产出空文件）。
        void addGame(const GameOut &g)
        {
            for (const Frame &fr : g.frames)
            {
                w_.b.add(fr, g.term);
                w_.endRow();
                if (w_.size() >= shard_rows_)
                {
                    written_.push_back(w_.flush());
                    ++part_;
                    setPath();
                }
            }
        }

        // 收尾：返回全部分片路径（一场都没写出来时为空，由调用方判）。
        std::vector<std::string> finish()
        {
            const std::string last = w_.flush();
            if (!last.empty())
            {
                written_.push_back(last);
            }
            return written_;
        }

    private:
        void setPath()
        {
            char buf[32];
            // ⚠️ 分片号补零：读侧按名字序拼分片，`blk2` 必须排在 `blk10` 前面。
            std::snprintf(buf, sizeof(buf), "blk%05d.bin", part_);
            w_.path = (std::filesystem::path(dir_) / buf).string();
        }

        std::string dir_;
        int shard_rows_ = 0;
        int part_ = 0;
        ShardWriter w_;
        std::vector<std::string> written_;
    };

} // namespace sts::collect

#endif // STS_LIGHTSPEED_SHARD_OUT_H
