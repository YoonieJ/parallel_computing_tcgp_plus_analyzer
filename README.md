# parallel_computing_tcgp_plus_analyzer

This repository contains the full final project submission for the Pokemon TCG Pocket probability analyzer.

## Repository Layout

- `parallel_computing_tcgp_plus_analyzer`: project summary and analyzer overview
- `final_project/`: source code, build files, input datasets, report PDF, and supporting project files

## Build

From inside `final_project/`:

```bash
cmake -S . -B build
cmake --build build
```

Or:

```bash
g++ -std=c++17 -fopenmp main.cpp FileParser.cpp GameSimulation.cpp -o TCGPPlus
```

See `final_project/README.md` for the full project description, benchmark mode, and usage notes.
