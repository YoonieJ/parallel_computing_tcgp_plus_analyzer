# TCG-P+: Pokemon TCG Pocket Probability Analyzer

TCG-P+ is a C++ project that tracks a Pokemon TCG Pocket game state, estimates likely outcomes, and surfaces move recommendations during play. It combines structured card data, deck input, visible opponent information, recursive lookahead, and OpenMP-parallel evaluation to help a player reason about a position.

## Overview

The project focuses on three core tasks:

1. Load and normalize card, deck, and meta-deck data from text files.
2. Let the user enter the current board state as a game progresses.
3. Score possible follow-up actions and estimate outcome quality with recursive simulation.

The current implementation uses a heuristic evaluator plus recursive decision-tree search. It also includes a Monte Carlo helper and OpenMP-based parallel regions for top-level simulation work and meta-deck filtering.

## Current Features

- Deck loading and validation for 20-card lists with a two-copy limit.
- Card-database parsing for Pokemon, Item, and Supporter cards.
- Evolution-link setup from `PrevEvo` and `NextEvo` card data.
- Interactive game-state updates for hand, active Pokemon, bench, attacks, and energy attachments.
- Opponent meta-deck filtering from visible Pokemon on board.
- Recursive move evaluation over attacks and bench switches.
- Move-by-move probability output for attacks, switches, and trainer-card usage.
- OpenMP parallelization for top-level decision-tree expansion, Monte Carlo sampling, and meta-deck filtering.

## Project Layout

- `main.cpp`: program entry point and round loop.
- `GameSimulation.cpp` / `GameSimulation.h`: gameplay input flow, evaluation logic, recursive simulation, and recommendation output.
- `FileParser.cpp` / `FileParser.h`: parsing helpers for card and deck text files.
- `PokemonCard.h`: data structures for cards, attacks, abilities, and game state.
- `Utils.h`: string normalization and tokenization helpers.
- `Constants.h`: deck and gameplay constants.
- `Cards.txt`: card database.
- `deck.txt`: player deck list.
- `metaDecks.txt`: candidate opponent meta-decks.

## Build

### With CMake

```bash
cmake -S . -B build
cmake --build build
```

### With g++

```bash
g++ -std=c++17 -fopenmp main.cpp FileParser.cpp GameSimulation.cpp -o TCGPPlus
```

### OpenMP Note

The project requires an OpenMP-capable compiler. On some macOS setups, the default Apple `clang++` does not support `-fopenmp` without an additional OpenMP toolchain.

## Benchmarking Parallelism

The project includes a built-in benchmark mode that compares serial and OpenMP-parallel execution on the same representative game state.

After building with CMake, run:

```bash
./build/TCGPPlus --benchmark
```

If you built directly with `g++`, run:

```bash
./TCGPPlus --benchmark
```

To test a specific thread count, set `OMP_NUM_THREADS` before running:

```bash
OMP_NUM_THREADS=8 ./TCGPPlus --benchmark
```

The benchmark prints two tables:

- `Decision Tree Benchmark`: compares `simulateDecisionTreeSequential(...)` against the parallel top-level decision-tree evaluation.
- `Monte Carlo Benchmark`: compares `monteCarloSimulationSequential(...)` against the OpenMP Monte Carlo loop.

For each thread count, the program reports:

- `Time (s)`: average runtime across repeated measurements
- `Speedup`: `T1 / Tp`
- `Efficiency`: `Speedup / p`
- `Score`: the resulting evaluation value for the benchmark state

This gives you direct evidence that parallel execution is reducing runtime. In practice, Monte Carlo should scale more cleanly than the decision tree because the simulation work is distributed more evenly across threads, while the decision tree is limited by the number of top-level branches.

## How To Run

1. Prepare `deck.txt` with one card name and count per line, for example:

```text
Bulbasaur, 2
Ivysaur, 2
Venusaur ex, 2
Weedle, 2
Beedrill, 2
Poke Ball, 2
Potion, 2
X Speed, 2
Sabrina, 2
Professor's Research, 2
```

2. Make sure `Cards.txt` and `metaDecks.txt` are present in the same directory as the executable.
3. Run the program.
4. Follow the prompts to:
   - load the deck,
   - enter the opening hand,
   - configure both boards,
   - record round-by-round actions,
   - inspect move probability output and meta-deck guesses.

## Simulation Model

The recommendation engine currently evaluates future states using:

- a heuristic board scorer based on HP, attached energy, available attacks, hand size, deck size, and status conditions,
- recursive branching over attacks and bench switches,
- optional Monte Carlo sampling for lightweight randomized draw exploration,
- OpenMP parallelization at the top level of the recursive tree.

This is best understood as an analysis assistant rather than a perfect game solver. The evaluator is intentionally lightweight so it can be called often during interactive play.

## Limitations

- The search space is still simplified compared with the full Pokemon TCG Pocket ruleset.
- The recursive tree currently expands attack and switch branches, not every possible legal action.
- The heuristic evaluator estimates position strength; it is not backed by a full rules engine.
- Opponent modeling is based on visible board information and candidate meta-decks, not hidden-card inference.

## References

1. [Game8 Pokemon TCG Pocket Guide](https://game8.co/games/Pokemon-TCG-Pocket/archives/483063)
2. [Bulbapedia: Type (TCG)](https://bulbapedia.bulbagarden.net/wiki/Type_(TCG))
