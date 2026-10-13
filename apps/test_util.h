//
// C++ 测试的最小设施 —— 形状抄 KataGo 的 `cpp/core/test.h`（那边也只有这一层）。
//
// 为什么不引 gtest/boost：本仓的其他目标一个第三方测试库都没有，引进来等于给"跑一次测试"
// 增加一条安装前置；而需要的东西其实就两件 —— **永远开着的断言**（Release 下 `assert` 会被
// `NDEBUG` 摘掉）与**失败时能看出差在哪一行**的比较。
//
// 用法见 `apps/scene-test.cpp`。
//
#ifndef STS_LIGHTSPEED_TEST_UTIL_H
#define STS_LIGHTSPEED_TEST_UTIL_H

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace testutil {

// 行内裁掉首尾空白（`diff` 用的比较口读；行尾空格不该算差异）。
inline std::string trim(const std::string &s)
{
    const std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
    {
        return {};
    }
    const std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

inline std::vector<std::string> splitLines(const std::string &s)
{
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line))
    {
        out.push_back(line);
    }
    return out;
}

inline void fatal(const std::string &msg)
{
    std::fprintf(stderr, "\n❌ %s\n", msg.c_str());
    std::fflush(stderr);
    std::exit(1);
}

// 永远开着的断言（不随 `NDEBUG` 消失）。
#define TEST_ASSERT(EX)                                                                          \
    do                                                                                           \
    {                                                                                            \
        if (!(EX))                                                                               \
        {                                                                                        \
            testutil::fatal(std::string("断言失败: ") + #EX + "\n    " + __FILE__ + ":" +        \
                            std::to_string(__LINE__));                                           \
        }                                                                                        \
    } while (0)

// 逐行比两段文本。**失败时打印第一条不同的行**（预期 / 实际并排 + 第一个不同的字符），
// 而不是甩一个 hash —— 一颗 hash 红了等于没说。
inline void expectText(const std::string &name, const std::string &actual,
                       const std::string &expected)
{
    const std::vector<std::string> a = splitLines(actual);
    const std::vector<std::string> e = splitLines(expected);
    const std::size_t n = std::max(a.size(), e.size());
    for (std::size_t i = 0; i < n; ++i)
    {
        const std::string al = i < a.size() ? trim(a[i]) : std::string("<缺行>");
        const std::string el = i < e.size() ? trim(e[i]) : std::string("<缺行>");
        if (al == el)
        {
            continue;
        }
        std::size_t col = 0;
        while (col < al.size() && col < el.size() && al[col] == el[col])
        {
            ++col;
        }
        std::fprintf(stderr, "\n❌ %s：第 %zu 行不同（0 起，第 %zu 个字符）\n", name.c_str(), i, col);
        std::fprintf(stderr, "    预期: %s\n", el.c_str());
        std::fprintf(stderr, "    实际: %s\n", al.c_str());
        std::fprintf(stderr, "    %s^\n", std::string(col + 4, ' ').c_str());
        std::fflush(stderr);
        std::exit(1);
    }
}

// 把一批 `名称 = 值` 收成一段文本（便于 `expectText` 与"两批必须相同"的对比）。
class Report
{
public:
    void add(const std::string &key, const std::string &value)
    {
        std::ostringstream os;
        os << key << " = " << value;
        lines_.push_back(os.str());
    }
    const std::vector<std::string> &lines() const { return lines_; }
    std::string text() const
    {
        std::string out;
        for (const std::string &l : lines_)
        {
            out += l;
            out += '\n';
        }
        return out;
    }

private:
    std::vector<std::string> lines_;
};

} // namespace testutil

#endif //STS_LIGHTSPEED_TEST_UTIL_H
