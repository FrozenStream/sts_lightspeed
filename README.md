# sts_lightspeed

`sts_lightspeed` is a *Slay the Spire* simulation and learning project built around a fast C++ game engine. The original project focused on reproducing the game's mechanics and random number generation, providing a standalone simulator for console play, large-scale simulation, and tree search. This version builds on that foundation with a policy-value neural network (NN), PUCT-based Monte Carlo Tree Search (MCTS), and a pipeline for collecting data, training models, and evaluating their decisions.

The goal is to study how learned policies and values can improve combat planning under a limited search budget. The simulator supplies the game rules and state transitions; the network learns which actions look promising and how valuable a position is; search combines those predictions with simulated consequences. The current learning pipeline focuses on **Ironclad combat**, including card play, target selection, potion use, ending turns, and card-selection decisions within a battle.

## From simulation to learning

The original simulator remains the foundation of the project. Game states can be copied and advanced through legal actions, allowing search to explore different sequences without running the game client. Existing UCT search also serves as a teacher for generating training examples and as a reference for evaluating learned agents.

The added neural network, `AZNet`, is implemented in PyTorch. It consumes a structured battle snapshot containing player resources and statuses, cards and card piles, monsters and their intents, relics, potions, and card-selection candidates. Learned embeddings represent entities such as cards and monsters, while numerical features describe the current position. A shared residual network feeds three outputs:

- **Action policy:** scores for playing cards, choosing targets, using or discarding potions, and ending the turn.
- **Card-selection policy:** scores for candidate cards when a combat effect requires a selection.
- **Value:** an estimate of the battle outcome under the configured terminal scoring rule.

The engine supplies legal-action masks so that policy decisions and search expansion use actions available in the current state. Python derives the observation layout from the C++ engine to keep training and simulation aligned.

## Neural network-guided PUCT search

PUCT combines the estimated value of an action with an exploration bonus weighted by the network's policy prior. At each decision, search repeatedly selects a path through the tree, simulates its actions with the C++ engine, expands a new position, and backs up its value. The network supplies priors for new branches and values for nonterminal leaves; terminal positions use their actual configured scores. This search uses neural leaf evaluation instead of random rollouts.

Root visit counts provide a search policy that can select the next action or become a training target. Self-play supports exploration through visit-based sampling, temperature, and root Dirichlet noise, along with configurable simulation budgets. Search therefore acts both as a decision-making procedure and as a source of examples for improving the network.

Simulation, tree traversal, scenario generation, and data collection run in C++. Multiple independent battles share a batched inference service, allowing network evaluations to be combined across searches. Models are trained in PyTorch and exported to ONNX; the C++ inference backend uses ONNX Runtime with CPU or CUDA execution.

## Data, training, and evaluation

The project supports both learning from the original search teacher and iterative learning from the network's own search:

1. **Generate battle scenarios.** Construct encounters with randomized decks, upgrades, relics, potions, health, and other resources appropriate to the scenario's floor.
2. **Collect decisions.** Use UCT teacher search or NN-guided PUCT self-play to record observations, legal actions, search visit distributions, and terminal outcomes in data shards.
3. **Train the network.** Fit policy predictions to search-derived targets and value predictions to terminal scores. Teacher and self-play data share a format and can be mixed; the default policy target is the most-visited action.
4. **Export for inference.** Convert the trained checkpoint to ONNX for use by the C++ search and evaluation code.
5. **Compare policies.** Evaluate random play, the UCT teacher, the network alone, and the network with MCTS on matched scenarios and held-out seeds.
6. **Iterate.** Collect new self-play data with the updated model, continue training with optional replay from recent rounds, and track candidates against the best model so far.

Collection recipes and model configuration are recorded with the artifacts, and training and evaluation use separate seed ranges. Paired evaluation helps measure policy changes on the same starting positions. Network-only results and search-assisted results are reported separately because they measure different decision-making procedures and computational budgets.

## Project layout

| Component | Role |
| --- | --- |
| [`src/`](src/) and [`include/`](include/) | Core game simulation, rules, and original search agents. |
| [`bindings/`](bindings/) | Python bindings and ONNX Runtime inference backend. |
| [`collect/`](collect/) | C++ scenario generation, teacher collection, PUCT self-play, and evaluation. |
| [`aznet.py`](aznet.py), [`azlayout.py`](azlayout.py), and [`azaction.py`](azaction.py) | Network architecture, observation layout, and action-space definitions. |
| [`collect_teacher.py`](collect_teacher.py) and [`collect_selfplay.py`](collect_selfplay.py) | Python entry points for producing training data. |
| [`train.py`](train.py) and [`onnxnet.py`](onnxnet.py) | Model training and ONNX export. |
| [`eval_gate.py`](eval_gate.py) and [`chain.sh`](chain.sh) | Paired policy evaluation and iterative self-play/training orchestration. |
| [`apps/`](apps/) | Console programs and C++ checks for the engine, scenarios, inference, and self-play. |
| [`realtest/`](realtest/) | CommunicationMod bridge for testing combat policies in the game client. |

## Current scope

The learning work targets individual Ironclad battles. The simulator also implements systems outside combat, but the combat network does not provide a learned strategy for an entire run, including map routing, shops, events, and card rewards. Synthetic battle scenarios provide broad training coverage, while performance in a complete run also depends on how those positions are reached.

The current simulator search follows the supplied state's RNG, including future draws. Search-assisted results therefore describe planning with that simulator information. The network observation represents card piles without their hidden order. This distinction matters when comparing simulator search, network-only play, and play in the actual game.

The project is an experimental platform for combining fast simulation with learned combat decisions. Its AlphaZero-style workflow refers to the policy/value/search feedback loop; *Slay the Spire* is a single-player game, so self-play here means the agent generating its own battle trajectories.

---

## Original simulator README

The original project description and historical build notes are preserved below. The current CMake configuration uses C++20 and requires ONNX Runtime; see [`CMakeLists.txt`](CMakeLists.txt) for the current build targets and dependencies.

For tree search and simulation of the popular rogue-like deckbuilder game Slay The Spire

**Features**
* c++ 17 compiled with gcc
* Standalone
* Designed to be 100% RNG accurate*
* Playable in console
* Speed: 1M random playouts in 5s with 16 threads
* Loading from save files (loading into combat currently only supported)
* Tree Search (best result, knowing the state of the game's rng)

**Planned Features**
* Tree search of possible game outcomes (not given the state of rng)

**Implementation Progress**
* All enemies
* All relics
* All Ironclad cards
* All colorless cards
* Everything outside of combat / all acts

**Getting Started**
* The project was built with Clion2021 and the [mingw64 toolchain](https://www.msys2.org/) on Windows 10
* The main target creates a simulator of the game that can be played in console.
* The test target creates a program with various commands that can be run, including random simulation
* Click the star button at the top of the repo :)

**Build tips**
* If your build fails with an error about not-return-only `constexpr` methods, ensure your compiler supports c++17.
* If CLion shows an error about not finding python libs when loading the cmake project, try opening CLion from the msys2 shell.
