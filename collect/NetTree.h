#include <optional>


namespace sts::collect
{
    struct Slot
    {
        std::optional<Tree> tree;
        TreeState state;
        ForwardService::Query query;
    };
}