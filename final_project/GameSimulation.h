#ifndef GAMESIMULATION_H
#define GAMESIMULATION_H

#include "PokemonCard.h"
#include <unordered_map>
#include <string>

// Returns a normalized score for the current game state.
double evaluateGameState(const GameState &state, int depth = 0);

// Samples lightweight randomized futures from the current state.
double monteCarloSimulation(const GameState &state, int numSimulations);

// Serial Monte Carlo baseline used for performance comparisons.
double monteCarloSimulationSequential(const GameState &state, int numSimulations);

// Loads candidate opponent decks for later filtering.
void loadAllMetaDecks(const std::string &filename);

// Filters opponent deck guesses from visible board information.
void updateMetaDeckGuesses(GameState &state);

// Handles the player's action step for the round.
void processRoundInput(GameState &state);

// Loads the player's deck list from disk.
void loadPresetDeck(const std::string &deckFile,
                    const std::unordered_map<std::string, Pokemon> &cardMap,
                    std::vector<Pokemon> &deck);

// Reports invalid deck sizes or copy counts.
void validateDeck(const std::vector<Pokemon> &deck);

// Moves the chosen opening hand from deck to hand.
void drawInitialHand(GameState &state);

// Parallel top-level recursive lookahead.
double simulateDecisionTree(const GameState &state, int depth);

// Sequential helper used by the recursive tree.
double simulateDecisionTreeSequential(const GameState &state, int depth);

// Displays the loaded deck before play begins.
void preStartConfiguration(const GameState &state);

// Collects setup information before the first round.
void preFirstRoundConfiguration(GameState &state);

// Records board state after the opening round.
void postFirstRoundUpdate(GameState &state, const std::unordered_map<std::string, Pokemon> &cardMap);

// Records the player's draw for a new round.
void preEveryRoundConfiguration(GameState &state);

// Records the opponent's visible post-round actions.
void postEveryRoundUpdate(GameState &state);

// Prints action-level probability estimates.
void printActionProbabilities(const GameState &state, int simulationDepth);

// Runs a reproducible serial-vs-parallel benchmark on representative states.
void runPerformanceBenchmarks(const std::unordered_map<std::string, Pokemon> &cardMap,
                              const std::vector<Pokemon> &deckTemplate,
                              int decisionTreeDepth = 7,
                              int monteCarloSamples = 200000,
                              int repetitions = 3);

#endif
