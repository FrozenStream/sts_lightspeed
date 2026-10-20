//
// Created by keega on 9/16/2021.
//

#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "combat/BattleContext.h"
#include "combat/Monster.h"
#include "combat/MonsterGroup.h"
#include "combat/Player.h"
#include "game/Game.h"
#include "sim/ConsoleSimulator.h"
#include "sim/PrintHelpers.h"
#include "sim/SimHelpers.h"
#include "sim/search/ScumSearchAgent2.h"

#include "slaythespire.h"
#include "action.h" // 动作层（枚举 / 编码 / 反解）—— `sts::collect`

#include "scenebuild_bind.h" // registerSceneBuild（纯 C++ 那半在 scenebuild.h）
#include "scenebuild.h"
#include "selfplay.h"
#include "teacher.h"
#include "eval.h"       // registerEval（配对评估触发器）
#include "bind.h"       // registerSelfplay / registerEval / registerTeacher 的声明
#include "zvalue.h"     // terminalValue
#include "net_eval.h"  // 推理后端的选择点（selfplay/eval 的 model 路径由它决定）

using namespace sts;

static constexpr std::pair<const char *, Event> kEventBindingNames[] = {
    {"INVALID", Event::INVALID},
    {"MONSTER", Event::MONSTER},
    {"REST", Event::REST},
    {"SHOP", Event::SHOP},
    {"TREASURE", Event::TREASURE},
    {"NEOW", Event::NEOW},
    {"OMINOUS_FORGE", Event::OMINOUS_FORGE},
    {"PLEADING_VAGRANT", Event::PLEADING_VAGRANT},
    {"ANCIENT_WRITING", Event::ANCIENT_WRITING},
    {"OLD_BEGGAR", Event::OLD_BEGGAR},
    {"BIG_FISH", Event::BIG_FISH},
    {"BONFIRE_SPIRITS", Event::BONFIRE_SPIRITS},
    {"COLOSSEUM", Event::COLOSSEUM},
    {"CURSED_TOME", Event::CURSED_TOME},
    {"DEAD_ADVENTURER", Event::DEAD_ADVENTURER},
    {"DESIGNER_IN_SPIRE", Event::DESIGNER_IN_SPIRE},
    {"AUGMENTER", Event::AUGMENTER},
    {"DUPLICATOR", Event::DUPLICATOR},
    {"FACE_TRADER", Event::FACE_TRADER},
    {"FALLING", Event::FALLING},
    {"FORGOTTEN_ALTAR", Event::FORGOTTEN_ALTAR},
    {"THE_DIVINE_FOUNTAIN", Event::THE_DIVINE_FOUNTAIN},
    {"GHOSTS", Event::GHOSTS},
    {"GOLDEN_IDOL", Event::GOLDEN_IDOL},
    {"GOLDEN_SHRINE", Event::GOLDEN_SHRINE},
    {"WING_STATUE", Event::WING_STATUE},
    {"KNOWING_SKULL", Event::KNOWING_SKULL},
    {"LAB", Event::LAB},
    {"THE_SSSSSERPENT", Event::THE_SSSSSERPENT},
    {"LIVING_WALL", Event::LIVING_WALL},
    {"MASKED_BANDITS", Event::MASKED_BANDITS},
    {"MATCH_AND_KEEP", Event::MATCH_AND_KEEP},
    {"MINDBLOOM", Event::MINDBLOOM},
    {"HYPNOTIZING_COLORED_MUSHROOMS", Event::HYPNOTIZING_COLORED_MUSHROOMS},
    {"MYSTERIOUS_SPHERE", Event::MYSTERIOUS_SPHERE},
    {"THE_NEST", Event::THE_NEST},
    {"NLOTH", Event::NLOTH},
    {"NOTE_FOR_YOURSELF", Event::NOTE_FOR_YOURSELF},
    {"PURIFIER", Event::PURIFIER},
    {"SCRAP_OOZE", Event::SCRAP_OOZE},
    {"SECRET_PORTAL", Event::SECRET_PORTAL},
    {"SENSORY_STONE", Event::SENSORY_STONE},
    {"SHINING_LIGHT", Event::SHINING_LIGHT},
    {"THE_CLERIC", Event::THE_CLERIC},
    {"THE_JOUST", Event::THE_JOUST},
    {"THE_LIBRARY", Event::THE_LIBRARY},
    {"THE_MAUSOLEUM", Event::THE_MAUSOLEUM},
    {"THE_MOAI_HEAD", Event::THE_MOAI_HEAD},
    {"THE_WOMAN_IN_BLUE", Event::THE_WOMAN_IN_BLUE},
    {"TOMB_OF_LORD_RED_MASK", Event::TOMB_OF_LORD_RED_MASK},
    {"TRANSMORGRIFIER", Event::TRANSMORGRIFIER},
    {"UPGRADE_SHRINE", Event::UPGRADE_SHRINE},
    {"VAMPIRES", Event::VAMPIRES},
    {"WE_MEET_AGAIN", Event::WE_MEET_AGAIN},
    {"WHEEL_OF_CHANGE", Event::WHEEL_OF_CHANGE},
    {"WINDING_HALLS", Event::WINDING_HALLS},
    {"WORLD_OF_GOOP", Event::WORLD_OF_GOOP},
};

// `Snap` / `SnapWriter` / `snapshotOf` 已搬到 `collect/snapshot.h`（2026-10-10）：
// 采集侧（selfplay / teacher）要自己算观测，不能只住在宿主 TU 里。
// 布局仍是**唯一一份**，`snapshot_layout()` 从这里导出；改布局 = 作废全部缓存。
// ⚠️ 它们现在住在 **`sts::collect`**（搬文件只改了 namespace、没改这些 `using`，而链接它们的 TU
//    一直没被真编过 ⇒ 2026-10-11 全量重编才暴露）。`snapshot_layout` 那个 lambda 也在 `sts::py` 里。
#include "snapshot.h" // Snap / snapshotOf（采集侧也要用）
using sts::collect::Snap;
using sts::collect::snapshotOf;
using sts::collect::snapshotDivisors;
using sts::collect::buildBattle;

// ⚠️ 模块名是 `_slaythespire`（**下划线开头**），而它住在一个同名的**包**里
//    （`bindings/slaythespire_pkg/__init__.py`）；`import slaythespire` 拿到的是那个包。
//    为什么绕这一层：那个包同时是"类型面"的落点（`_slaythespire.pyi` 由 stubgen 生成在包里），
//    而包名 = 导入名是既有约定（历史原因见 `notes.md` §5-21：链 libtorch 时代必须先 `import torch`）。
//    ⚠️ 这里改名后，`PyInit__slaythespire` 必须与文件名一致 ⇒ CMake 里 `OUTPUT_NAME _slaythespire`。
PYBIND11_MODULE(_slaythespire, m)
{
    m.doc() = "pybind11 example plugin"; // optional module docstring

    // ── 推理后端（**只有一个**）──────────────────────────────────────
    // 报给 Python 只为"挡旧扩展"：`netarchive.py` 校验它必须是 `onnx` —— 链 libtorch 的老
    // `.pyd`/`.so` 会报 `torch`，于是采集侧当场拒绝起跑，而不是拿 `.onnx` 去喂 TorchScript。
    m.attr("NET_BACKEND") = "onnx";
    // ⚠️ `terminalValue`（值口径）住在 `collect/zvalue.cpp`，**不经绑定出口**：
    // 自对弈/教师/评估都在 C++ 内直接读它，Python 只需要 `zspec.z_from`（同口径的列侧实现）。

    // ── 类型预注册 ──────────────────────────────────────────────
    // pybind11 在 .def()/.value() 那一刻就把类型名写进 __text_signature__。
    // 被引用的类型若此时尚未注册，签名会退化成 C++ 名（sts::Card 之类），
    // pybind11-stubgen 解析不了 → 该处类型丢失成 `...`。
    // 所以先把所有类型注册一遍；各自的 .def()/.value() 仍留在原处。
    pybind11::enum_<search::ActionType> actionTypeEnum(m, "ActionType");
    pybind11::enum_<GameOutcome> gameOutcome(m, "GameOutcome");
    pybind11::enum_<Outcome> battleOutcome(m, "Outcome");
    pybind11::enum_<InputState> inputStateEnum(m, "InputState");
    pybind11::enum_<CardSelectTask> cardSelectTaskEnum(m, "CardSelectTask");
    pybind11::enum_<ScreenState> screenState(m, "ScreenState");
    pybind11::enum_<Event> eventEnum(m, "Event");
    pybind11::enum_<CharacterClass> characterClass(m, "CharacterClass");
    pybind11::enum_<Room> roomEnum(m, "Room");
    pybind11::enum_<CardRarity> cardRarity(m, "CardRarity");
    pybind11::enum_<CardColor> cardColor(m, "CardColor");
    pybind11::enum_<CardType> cardType(m, "CardType");
    pybind11::enum_<CardId> cardId(m, "CardId");
    pybind11::enum_<MonsterEncounter> meEnum(m, "MonsterEncounter");
    pybind11::enum_<RelicId> relicEnum(m, "RelicId");
    pybind11::enum_<RelicTier> relicTierEnum(m, "RelicTier");
    // 药水枚举：**用头文件里的名字表循环填**（43 项，手抄必出错）。
    // `potionEnumNames[]`（`include/constants/Potions.h`）的下标 == 枚举值。
    pybind11::enum_<Potion> potionEnum(m, "Potion");
    // 状态 / 怪 id 枚举：快照里这几列都是裸 `int`，绑成枚举后 Python 侧才有语义
    // （`sts.MonsterId(4).name` == "AWAKENED_ONE"）。填表循环在文末。
    pybind11::enum_<MonsterId> monsterIdEnum(m, "MonsterId");
    pybind11::enum_<MonsterMoveId> monsterMoveEnum(m, "MonsterMoveId");
    pybind11::enum_<PlayerStatus> playerStatusEnum(m, "PlayerStatus");
    pybind11::enum_<MonsterStatus> monsterStatusEnum(m, "MonsterStatus");

    pybind11::class_<Card> card(m, "Card");
    pybind11::class_<RelicInstance> relic(m, "Relic");
    pybind11::class_<Player> player(m, "Player");
    pybind11::class_<Monster> monster(m, "Monster");
    pybind11::class_<MonsterGroup> monsterGroup(m, "MonsterGroup");
    pybind11::class_<GameContext> gameContext(m, "GameContext");
    pybind11::class_<Map> map(m, "SpireMap");
    pybind11::class_<search::Action> action(m, "Action",
                                            "C++ search::Action — the Python-facing action type (replaces raw bits).");
    pybind11::class_<BattleContext> battleContext(m, "BattleContext");

    // 采集三件套：自对弈热循环（K 棵独立树 + 进程内一次前向）、按层造局面、教师采集
    // （纯 C++ UCT）。三者共用同一套分片容器（观测/落盘都在 C++ 侧，不经 Python）。
    // ⚠️ **必须放在上面那批类型注册之后**：pybind 在 `.def()` 那一刻就把类型名写进
    //    `__text_signature__`，被引用的类型若还没注册，签名退化成 C++ 名（`sts::BattleContext`），
    //    `pybind11-stubgen` 解析不了 ⇒ 那几条 `.pyi` 的类型就丢了（Pylance 全变 `...`）。
    sts::collect::registerSelfplay(m);
    sts::collect::registerSceneBuild(m);
    sts::collect::registerTeacher(m);
    sts::collect::registerEval(m);

    m.def("evaluate_end_state", &search::BattleScumSearcher2::evaluateEndState, pybind11::arg("bc"),
          "引擎的终局分（`evaluateEndState`）：赢 = 100*(35 + curHp + 4*药水 - 0.01*回合)，"
          "输 = 10*(1-非小怪血比) - 活怪数 - 0.2*能量浪费 + 0.03*抽牌 + 2*药水 + 0.2*回合。");
    // 批量快照：一次跨语言调用把 k 个 BattleContext 的快照按行写进 (k, snapshot_len) float32。
    // 逐叶调 `get_snapshot_np` 是 k 次分配 + Python 侧 `np.stack`。走**归一化**出口（同 `get_snapshot_np`）。
    m.def(
        "batch_snapshot",
        [](const pybind11::sequence &ctxs)
        {
            const auto k = static_cast<pybind11::ssize_t>(ctxs.size());
            pybind11::array_t<float> out(
                std::vector<pybind11::ssize_t>{k, static_cast<pybind11::ssize_t>(Snap::snapshotLen)});
            auto buf = out.mutable_unchecked<2>();
            for (pybind11::ssize_t j = 0; j < k; ++j)
            {
                const auto v = snapshotOf(ctxs[j].cast<const BattleContext &>());
                for (int i = 0; i < Snap::snapshotLen; ++i)
                {
                    buf(j, i) = v[i];
                }
            }
            return out;
        },
        pybind11::arg("contexts"), "batch of get_snapshot_np: (k, snapshot_len) float32, row j = contexts[j]");

    // 快照布局：读本文件开头的 `Snap` —— **唯一一份**（布局与生产者同住一个 TU）。
    // Python 侧一律从这里推导下标，**不要在 Python 里硬编码偏移**。
    m.def(
        "snapshot_layout",
        []()
        {
            pybind11::dict d;
            // 初始化一次除数表：`snapshot_layout()` 在训练导入时就会被调用，不能等第一场。
            static const bool schema_ready = []
            {
                BattleContext bc = buildBattle(0, MonsterEncounter::CULTIST, 1, 0);
                (void)snapshotOf(bc);
                return true;
            }();
            (void)schema_ready;
            d["len"] = Snap::snapshotLen;
            d["player_len"] = Snap::playerLen;
            d["player_off"] = Snap::playerOff;
            d["counts_len"] = Snap::countsLen;
            d["counts_off"] = Snap::countsOff;
            d["hand_slots"] = Snap::handSlots;
            d["hand_fields"] = Snap::handFields;
            d["hand_off"] = Snap::handOff;
            d["pile_slots"] = Snap::pileSlots;
            d["pile_fields"] = Snap::pileFields;
            d["draw_off"] = Snap::drawOff;
            d["discard_off"] = Snap::discardOff;
            d["exhaust_off"] = Snap::exhaustOff;
            d["deck_slots"] = Snap::deckSlots;
            d["deck_fields"] = Snap::deckFields;
            d["deck_off"] = Snap::deckOff;
            d["monster_slots"] = Snap::monsterSlots;
            d["monster_fields"] = Snap::monsterFields;
            d["monster_off"] = Snap::monsterOff;
            d["player_attr_len"] = Snap::playerAttrLen;
            d["player_attr_off"] = Snap::playerAttrOff;
            d["player_counter_len"] = Snap::playerCounterLen;
            d["player_counter_off"] = Snap::playerCounterOff;
            d["player_status_len"] = Snap::playerStatusLen;
            d["player_status_off"] = Snap::playerStatusOff;
            d["monster_status_len"] = Snap::monsterStatusLen;
            d["monster_status_off"] = Snap::monsterStatusOff;
            d["card_select_slots"] = Snap::cardSelectSlots;
            d["card_select_fields"] = Snap::cardSelectFields;
            d["card_select_off"] = Snap::cardSelectOff;
            d["select_meta_len"] = Snap::selectMetaLen;
            d["select_meta_off"] = Snap::selectMetaOff;
            d["run_ctx_len"] = Snap::runCtxLen;
            d["run_ctx_off"] = Snap::runCtxOff;
            d["relic_len"] = Snap::relicLen;
            d["relic_off"] = Snap::relicOff;
            d["potion_meta_len"] = Snap::potionMetaLen;
            d["potion_meta_off"] = Snap::potionOff;
            d["potion_slots"] = Snap::potionSlots;
            d["potion_slot_off"] = Snap::potionSlotOff;
            d["encoded_card_id_types"] = Snap::encodedCardIdTypes;
            d["monster_id_types"] = Snap::monsterIdTypes;
            d["monster_move_id_types"] = Snap::monsterMoveIdTypes;
            d["input_state_types"] = Snap::inputStateTypes;
            d["card_select_task_types"] = Snap::cardSelectTaskTypes;
            d["stance_types"] = Snap::stanceTypes;
            d["orange_pellets_types"] = Snap::orangePelletsTypes;
            d["potion_types"] = Snap::potionTypes;
            d["divisors"] = std::vector<std::uint16_t>(snapshotDivisors().begin(), snapshotDivisors().end());
            std::vector<std::string> blocks;
            blocks.reserve(Snap::blocks.size());
            for (const auto &b : Snap::blocks)
            {
                blocks.emplace_back(b.name);
            }
            d["obs_blocks"] = blocks;
            return d;
        },
        "fixed-length battle snapshot layout: block lengths and start offsets");

    // 动作索引空间的**结构**（权威 = `slaythespire.h` 里那几行 `constexpr`）：
    // 段起点 / 段宽 / 步长一律从这里取，Python 侧**不手抄**。
    m.def(
        "action_layout",
        []()
        {
            pybind11::dict d;
            d["hand_slots"] = sts::collect::handSlots;
            d["potion_slots"] = sts::collect::potionSlots;
            d["monster_slots"] = sts::collect::maxMonsters;
            d["hand_stride"] = sts::py::handStride;     // 单张牌 = 1 无目标 + 怪槽
            d["potion_stride"] = sts::py::potionStride; // 单瓶药 = 1 无目标 + 怪槽 + 1 丢弃
            d["potion_base"] = sts::py::potionBase;     // 药水段起点 = 手牌段宽度
            d["end_turn"] = sts::py::endTurnIdx;
            d["count"] = sts::py::unifiedActionCount;
            return d;
        },
        "unified action index space: segment widths, strides and offsets");

    // --- 搜索动作：直接暴露 search::Action（Python 侧不再用裸 bits）---

    action.def(pybind11::init<std::uint32_t>(), pybind11::arg("bits"), "Action(bits) == C++ Action(std::uint32_t)")
        .def(pybind11::init<search::ActionType>(), pybind11::arg("action_type"),
             "Action(action_type) == C++ Action(ActionType)")
        .def(pybind11::init<search::ActionType, int>(), pybind11::arg("action_type"), pybind11::arg("idx1"),
             "Action(action_type, idx1) == C++ Action(ActionType, int)")
        .def(pybind11::init<search::ActionType, int, int>(), pybind11::arg("action_type"), pybind11::arg("idx1"),
             pybind11::arg("idx2"), "Action(action_type, idx1, idx2) == C++ Action(ActionType, int, int)")
        .def_property_readonly(
            "bits", [](const search::Action &a)
            { return a.bits; }, "raw bit encoding (== C++ Action::bits)")
        .def(
            "execute", [](const search::Action &a, BattleContext &bc)
            { a.execute(bc); }, pybind11::arg("bc"),
            "C++ Action::execute(bc)")
        .def(
            "__eq__",
            [](const search::Action &a, pybind11::object other)
            {
                return pybind11::isinstance<search::Action>(other) && a == other.cast<search::Action>();
            },
            pybind11::is_operator())
        .def(
            "__ne__",
            [](const search::Action &a, pybind11::object other)
            {
                return !(pybind11::isinstance<search::Action>(other) && a == other.cast<search::Action>());
            },
            pybind11::is_operator())
        .def("__hash__", [](const search::Action &a)
             { return std::hash<std::uint32_t>{}(a.bits); })
        .def("__repr__", [](const search::Action &a)
             {;
            static const char *typeNames[] = {"CARD", "POTION", "SINGLE_CARD_SELECT", "MULTI_CARD_SELECT", "END_TURN"};
            const auto t = static_cast<int>(a.getActionType());
            const char *name = (t >= 0 && t <= 4) ? typeNames[t] : "INVALID";
            std::ostringstream os;
            os << "<Action " << name << " src=" << a.getSourceIdx() << " tgt=" << a.getTargetIdx() << " bits=0x"
               << std::hex << a.bits << ">";
            return os.str(); });

    battleContext.def(pybind11::init<>(), "default construct")
        .def(pybind11::init<const BattleContext &>(), pybind11::arg("other"), "copy construct")
        .def(
            "init", [](BattleContext &bc, const GameContext &gc)
            { bc.init(gc); }, pybind11::arg("gc"),
            "BattleContext::init(gc); requires gc.screen_state == ScreenState::BATTLE.\n"
            "NOT a constructor and does not reset state: call at most once per object "
            "(a second init on the same object is UB). Open a new battle with a new object.")
        .def(
            "init",
            [](BattleContext &bc, const GameContext &gc, MonsterEncounter encounter)
            { bc.init(gc, encounter); },
            pybind11::arg("gc"), pybind11::arg("encounter"),
            "BattleContext::init(gc, encounter); call at most once per object")
        .def("__repr__", [](const BattleContext &)
             { return "<BattleContext>"; })
        .def_readonly("outcome", &BattleContext::outcome)
        .def_readonly("input_state", &BattleContext::inputState)
        .def_property_readonly("player_hp", [](const BattleContext &bc)
                               { return bc.player.curHp; })
        .def_property_readonly("player_max_hp", [](const BattleContext &bc)
                               { return bc.player.maxHp; })
        .def_readonly("turn", &BattleContext::turn)
        // 计数类的**引擎真值**：快照里它们是 ÷30 的网络输入，读不出个数 ⇒ 单独开访问器。
        .def_property_readonly("monsters_alive", [](const BattleContext &bc)
                               { return bc.monsters.monstersAlive; })
        .def_property_readonly("master_deck_size", [](const BattleContext &bc)
                               { return bc.masterDeckSize; })
        // 怪物读数**只有两条路**：快照的怪物块（含 id / 意图 / targetable），或活对象
        // `bc.monsters[i]` 上那几个 def_readwrite。**不开 `get_monster_*` 访问器** —— 多一套
        // 读法只是多一处会漂开的副本。
        // ⚠️ 快照已归一化 ⇒ 它的怪物块**只服务网络**，不能当引擎真值读；
        //    要真值就抛 `bc.monsters[i]`。
        .def(
            "get_snapshot", [](const BattleContext &bc)
            { return snapshotOf(bc); },
            (std::string("one-shot fixed-length snapshot of the whole battle, **NORMALIZED** (float).\n"
                         "len = sts.snapshot_layout()['len']. This is the network input vector: every numeric\n"
                         "column is already divided by its divisor AT THE PRODUCER (see `SnapWriter` in this\n"
                         "file) -- do NOT divide again, and do NOT read it back as engine truth (use the\n"
                         "live-object accessors instead: player_hp / monster_count / monsters_alive /\n"
                         "master_deck_size / monsters[i].cur_hp ...). Index / nominal columns (cardId,\n"
                         "monsterId, intentMoveId, inputState, cardSelectTask, stance, potion ids) are\n"
                         "emitted verbatim so they stay exact lookup keys.\n"
                         "Layout (cardId is NNInterface-style id*2+upgraded; specialData = persistent\n"
                         "counters for Searing Blow / Rampage / Genetic Algorithm / Ritual Dagger):\n"
                         "  [0..6]     player: curHp, maxHp, block, energy, turn, inputState, cardSelectTask\n"
                         "  [8..14]    counts: monsterCount, monstersAlive, handSize, drawSize, discardSize,\n"
                         "             exhaustSize, deckSize. monsterCount INCLUDES CORPSES (it never shrinks);\n"
                         "             monstersAlive is the victory condition, deckSize is the master deck's\n"
                         "             size (the deck slots alone cannot carry it -- mean pooling drops it).\n") +
             "  [" + std::to_string(Snap::handOff) + ".." + std::to_string(Snap::drawOff - 1) +
             "] hand: 10 slots x [cardId, costForTurn, specialData, playable] (empty=0)\n" + "  [" +
             std::to_string(Snap::drawOff) + ".." + std::to_string(Snap::discardOff - 1) +
             "] draw pile: " + std::to_string(Snap::pileSlots) +
             " slots x [cardId], CONTAINER order, zero-padded.\n"
             "             The slot ORDER carries no meaning: the net pools a pile as a SET\n"
             "             (per-card embed -> mean over non-empty slots) -- no ordering claim is\n"
             "             made here and nothing is truncated (128 >= any pile the engine builds).\n"
             "             NO cost/specialData for the same reason: per-slot scalars would be\n"
             "             position-indexed features.\n" +
             "  [" + std::to_string(Snap::discardOff) + ".." + std::to_string(Snap::exhaustOff - 1) +
             "] discard pile: same layout (container order, not recency)\n" + "  [" + std::to_string(Snap::exhaustOff) +
             ".." + std::to_string(Snap::deckOff - 1) + "] exhaust pile: same layout\n" + "  [" +
             std::to_string(Snap::deckOff) + ".." + std::to_string(Snap::monsterOff - 1) +
             "] master deck: " + std::to_string(Snap::deckSlots) +
             " slots x [cardId], ORDER-IRRELEVANT (per-card embed -> mean, like the piles).\n"
             "             NOT redundant with the piles: those hold battle-made temp/status cards\n"
             "             (Shiv/Wound/Dazed...) and shift as the battle progresses; this block is\n"
             "             the constant 'cards I own' view (same set as gc.deck on the run side).\n" +
             "  [" + std::to_string(Snap::monsterOff) + ".." + std::to_string(Snap::playerAttrOff - 1) +
             "] monsters: 5 x [monsterId, intentMoveId, hp, maxHp, block, targetable,\n"
             "             intentDamage, intentHits, miscInfo]\n"
             "             (empty=0). slots are NEVER compacted -- a dead monster keeps its slot\n"
             "             (monsterCount includes corpses). targetable = !isDeadOrEscaped() is the\n"
             "             ONLY dead/gone column; WHY a monster cannot be targeted is carried by\n"
             "             its INTENT: on the very frame it goes down, an Awakened One phase 1 /\n"
             "             Darkling gets AWAKENED_ONE_REBIRTH / DARKLING_REGROW, and an escaping\n"
             "             Looter/Mugger sits on LOOTER_ESCAPE / MUGGER_ESCAPE. miscInfo is the\n"
             "             engine's per-monster scratch int (Darkling regrow countdown, Champ\n"
             "             phase 2, ...).\n"
             "             intentDamage is the PER-HIT damage actually dealt to the player, already\n"
             "             including monster Strength, monster Weak, player Vulnerable/Intangible/Wrath\n"
             "             (Monster::calculateDamageToPlayer). Total incoming = intentDamage * intentHits.\n" +
             "  [" + std::to_string(Snap::playerAttrOff) + ".." + std::to_string(Snap::playerCounterOff - 1) +
             "] player attrs: stance, orbSlots, energyPerTurn\n" + "  [" + std::to_string(Snap::playerCounterOff) +
             ".." + std::to_string(Snap::playerStatusOff - 1) +
             "] player turn counters: cardsPlayed, attacksPlayed, skillsPlayed, cardsDiscarded,\n"
             "             cardDrawPerTurn, timesDamagedThisCombat, penNibCounter, happyFlowerCounter,\n"
             "             incenseBurnerCounter, inkBottleCounter, nunchakuCounter, sundialCounter,\n"
             "             orangePelletsCardTypesPlayed (bitset<3> as 0..7, NOMINAL\n"
             "             category -- index is the 3-bit combination, not a magnitude)\n" +
             "  [" + std::to_string(Snap::playerStatusOff) + ".." + std::to_string(Snap::monsterStatusOff - 1) +
             "] player statuses: 87 x PlayerStatus (index = enum value)\n" + "  [" +
             std::to_string(Snap::monsterStatusOff) + ".." + std::to_string(Snap::cardSelectOff - 1) +
             "] monster statuses: 5 x 43 x MonsterStatus (index = enum value)\n" + "  [" +
             std::to_string(Snap::cardSelectOff) + ".." + std::to_string(Snap::selectMetaOff - 1) +
             "] card-select candidates: " + std::to_string(Snap::cardSelectSlots) +
             " slots x [cardId, cost, costForTurn, specialData]\n"
             "             (source follows cardSelectTask; slot k == select_card(k), CONTAINER order,\n"
             "              so it is indexed differently from the pile blocks above; the width EQUALS\n"
             "              pileSlots because the candidate source is a whole pile)\n"
             "             A NON-ZERO slot means k IS a legal select action; illegal candidates\n"
             "             (Exhume itself, non-upgradable for Armaments, ...) are painted 0 IN PLACE.\n" +
             "  [" + std::to_string(Snap::selectMetaOff) + ".." + std::to_string(Snap::runCtxOff - 1) +
             "] select meta: candidateCount, pickCount, canPickZero, canPickAnyNumber\n"
             "             (candidateCount = slots laid out, NOT the number of legal actions;\n"
             "              CODEX lays out 3 but its legal set is {0,1,2,3}, k=3 = 'skip')\n" +
             "  [" + std::to_string(Snap::runCtxOff) + ".." + std::to_string(Snap::relicOff - 1) +
             "] run context: floor, gold.\n"
             "             (Run-level constants: identical in every frame of a battle.\n"
             "              Deliberately NO map block, NO isElite/isBoss and NO act: node context\n"
             "              does not change how a battle should be played, the elite/boss bit is\n"
             "              already carried by the monster_id embedding, and act is a function\n"
             "              of floor.)\n" +
             "  [" + std::to_string(Snap::relicOff) + ".." + std::to_string(Snap::potionOff - 1) +
             "] relics: " + std::to_string(Snap::relicLen) +
             " x 0/1, MULTI-HOT,\n"
             "             index = RelicId enum value. Read from `bc.player.relicBits0/1/2`\n"
             "             (the battle-side copy), so MCTS clones carry it -- NOT `gc.relics`.\n" +
             "  [" + std::to_string(Snap::potionOff) + ".." + std::to_string(Snap::potionSlotOff - 1) +
             "] potion meta: potionCount, potionCapacity\n" + "  [" + std::to_string(Snap::potionSlotOff) + ".." +
             std::to_string(Snap::snapshotLen - 1) + "] potions: " + std::to_string(Snap::potionSlotLen) +
             " x potion id, ONE PER SLOT (0 = empty).\n"
             "             Slot-addressed like the hand, because the action space is\n"
             "             (slot*7 + target) -- a bag multihot cannot tell two distinct\n"
             "             potions apart.\n")
                .c_str())
        .def(
            "get_snapshot_np",
            [](const BattleContext &bc)
            {
                const auto v = snapshotOf(bc);
                auto arr = pybind11::array_t<float>(static_cast<pybind11::ssize_t>(v.size()));
                std::copy(v.begin(), v.end(), arr.mutable_data());
                return arr;
            },
            "numpy version of get_snapshot (float32, **ALREADY NORMALIZED** = network input)")
        .def_property_readonly_static("unified_action_count",
                                      [](pybind11::object)
                                      { return sts::py::unifiedActionCount; })
        .def_property_readonly_static("end_turn_idx", [](pybind11::object)
                                      { return sts::py::endTurnIdx; })
        .def(
            "clone",
            [](const BattleContext &bc)
            {
                return std::make_unique<BattleContext>(bc); // 拷贝构造，与 C++ 一致
            },
            "copy of this battle (== BattleContext(other)); 便于 MCTS / AlphaZero 分支")
        // --- 搜索 ---
        // --- 统一动作索引（实现 = `collect/action.h`）---
        .def("enumerate_actions", sts::collect::enumerateActions,
             "all legal actions for current state as Action objects (C++ authoritative enumeration)")
        .def("legal_actions_with_idx", sts::collect::legalActionsWithIdx,
             "[(Action, unified_action_idx), ...] for the current state, in enumeration order.\n"
             "The pair exists so a policy can argmax logits[mask] without doing index arithmetic\n"
             "in Python; idx follows `actionToIdx` (raises when unmappable).")
        .def_readwrite("player", &BattleContext::player)
        .def_readwrite("monsters", &BattleContext::monsters);

    player.def_readwrite("block", &Player::block)
        .def_readwrite("cur_hp", &Player::curHp)
        .def_readwrite("max_hp", &Player::maxHp)
        .def_readwrite("energy", &Player::energy);

    monster.def_readwrite("idx", &Monster::idx)
        .def_readwrite("cur_hp", &Monster::curHp)
        .def_readwrite("max_hp", &Monster::maxHp)
        .def_readwrite("block", &Monster::block)
        // 只读真值：快照里那几列是归一化的（÷500 / ÷10…），读不出原值。
        .def_readonly("id", &Monster::id);

    monsterGroup.def_property_readonly("count", &sts::py::getMonsterCount)
        .def("__len__", &sts::py::getMonsterCount)
        .def("__getitem__", &sts::py::getMonster, pybind11::return_value_policy::reference_internal)
        .def(
            "__iter__",
            [](const MonsterGroup &self)
            {
                // ⚠️ 末尾指针只能写 `begin() + monsterCount`：`&arr[monsterCount]` 在
                //    `monsterCount == arr.size()`（= 5 怪上限，`LOTS_OF_SLIMES` 那种）时是
                //    **越界元素访问** ⇒ MSVC Debug 的 `std::array::operator[]` 当场 abort
                //    （实测 rc=3）。`begin() + n`（n ≤ size）是良定义的 one-past-the-end。
                return pybind11::make_iterator(self.arr.begin(), self.arr.begin() + self.monsterCount);
            },
            pybind11::keep_alive<0, 1>());

    gameContext
        .def(pybind11::init<CharacterClass, std::uint64_t, int>(), pybind11::arg("character_class"),
             pybind11::arg("seed"), pybind11::arg("ascension_level"))
        .def_property_readonly("encounter", [](const GameContext &gc)
                               { return gc.info.encounter; })
        .def_property_readonly(
            "deck", [](const GameContext &gc)
            { return std::vector(gc.deck.cards.begin(), gc.deck.cards.end()); },
            "returns a copy of the list of cards in the deck")
        // 随机升级 k 张可升级的牌 —— 引擎自带（`Deck::upgradeRandomCards`，Whetstone /
        // War Paint 那类"随机升级"效果走的就是它）：从可升级集合里洗牌取前 k 张，用
        // `gc.miscRng`（确定性、随局可复现）。
        // 药水：**唯一一个能往局里塞指定药水的入口**。
        // ⚠️ 药水槽**仍然没有读接口**：战斗里要读库存就看快照的药水块
        //    （`snapshot_layout()['potion_meta_off']`），不要指望 `gc.potions`。
        .def_property_readonly(
            "potions", [](const GameContext &gc)
            { return std::vector<Potion>(gc.potions.begin(), gc.potions.end()); },
            "copy of the potion slot array (Potion enum; INVALID / EMPTY_POTION_SLOT = empty).\n"
            "专为「场景指纹」与诊断加的 —— 战斗侧读库存请看快照的药水块。")
        .def_property_readonly(
            "relics", [](const GameContext &gc)
            { return std::vector(gc.relics.relics); },
            "returns a copy of the list of relics")
        .def(
            "__repr__",
            [](const GameContext &gc)
            {
                std::ostringstream oss;
                oss << "<" << gc << ">";
                return oss.str();
            },
            "returns a string representation of the GameContext");

    gameContext.def_readwrite("outcome", &GameContext::outcome)
        .def_readwrite("act", &GameContext::act)
        .def_readwrite("floor_num", &GameContext::floorNum);

    gameContext.def_readwrite("seed", &GameContext::seed)
        .def_readwrite("boss", &GameContext::boss);

    gameContext.def_readwrite("cur_hp", &GameContext::curHp)
        .def_readwrite("max_hp", &GameContext::maxHp)
        .def_readwrite("gold", &GameContext::gold);

    gameContext.def_readwrite("potion_chance", &GameContext::potionChance)
        .def_property_readonly(
            "map", [](GameContext &gc) -> Map &
            { return *gc.map; }, pybind11::return_value_policy::reference_internal,
            "readonly access to the current act map");

    relic.def_readwrite("id", &RelicInstance::id).def_readwrite("data", &RelicInstance::data);

    map.def(pybind11::init<std::uint64_t, int, int, bool>());
    map.def("__repr__", [](const Map &m)
            { return m.toString(true); });

    card.def(pybind11::init<CardId>(), pybind11::arg("card_id"))
        .def(
            "__repr__",
            [](const Card &c)
            {
                std::string s("<slaythespire.Card ");
                s += c.getName();
                if (c.isUpgraded())
                {
                    s += '+';
                    if (c.id == sts::CardId::SEARING_BLOW)
                    {
                        s += std::to_string(c.getUpgraded());
                    }
                }
                return s += ">";
            },
            "returns a string representation of a Card")
        .def("upgrade", &Card::upgrade)
        .def_readwrite("misc", &Card::misc,
                       "value internal to the simulator used for things like ritual dagger damage");

    card.def_property_readonly("id", &Card::getId)
        .def_property_readonly("upgraded", &Card::isUpgraded)
        .def_property_readonly("type", &Card::getType);

    inputStateEnum.value("CARD_SELECT", InputState::CARD_SELECT);

    for (int i = 0; i < (int)(sizeof(potionEnumNames) / sizeof(potionEnumNames[0])); ++i)
    {
    }

    // Event 枚举：从 `kEventBindingNames` 铺，顺序 = 枚举值顺序。
    for (const auto &e : kEventBindingNames)
    {
    }

    // PlayerStatus / MonsterStatus / MonsterId / MonsterMoveId：同样用名字表循环填（下标 == 枚举值）。
    // 这几张表也就是快照块宽度的来源（见 `Snap` 里的 `sizeof(...)`）⇒ 名字与宽度同源。
    for (int i = 0; i < (int)(sizeof(playerStatusEnumStrings) / sizeof(playerStatusEnumStrings[0])); ++i)
    {
    }
    for (int i = 0; i < (int)(sizeof(monsterIdStrings) / sizeof(monsterIdStrings[0])); ++i)
    {
    }
    for (int i = 0; i < (int)(sizeof(monsterMoveStrings) / sizeof(monsterMoveStrings[0])); ++i)
    {
    }

#ifdef VERSION_INFO
    m.attr("__version__") = MACRO_STRINGIFY(VERSION_INFO);
#else
    m.attr("__version__") = "dev";
#endif
}
