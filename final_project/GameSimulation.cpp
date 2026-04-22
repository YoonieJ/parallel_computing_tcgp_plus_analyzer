#include "GameSimulation.h"
#include "Constants.h"
#include "Utils.h"
#include <iostream>
#include <algorithm>
#include <random>
#include <limits>
#include <omp.h>
#include <fstream>
#include <cctype>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <initializer_list>
#include "FileParser.h"
#include <unordered_set>
#include <sstream>
#include <unordered_map>

std::vector<std::string> allMetaDecks;

namespace {

double clampProbability(double value) {
    return std::max(0.0, std::min(1.0, value));
}

int countAttachedEnergy(const Pokemon &pokemon) {
    int totalEnergy = 0;
    for (const auto &energy : pokemon.attachedEnergy) {
        totalEnergy += energy.amount;
    }
    return totalEnergy;
}

double scorePokemon(const Pokemon &pokemon, bool isActive) {
    if (pokemon.name.empty()) {
        return 0.0;
    }

    double score = static_cast<double>(pokemon.hp);
    score += 12.0 * countAttachedEnergy(pokemon);
    score += 8.0 * static_cast<double>(pokemon.skills.size());
    score += pokemon.isEx ? 10.0 : 0.0;
    score += isActive ? 12.0 : 6.0;

    if (pokemon.isPoisoned) {
        score -= 15.0;
    }
    if (pokemon.isParalyzed) {
        score -= 20.0;
    }

    return score;
}

void applySkillToState(GameState &state, const Skill &skill) {
    state.opponentActivePokemon.hp -= skill.dmg;
    if (state.opponentActivePokemon.hp < 0) {
        state.opponentActivePokemon.hp = 0;
    }

    if (skill.specialEffect.paralyzeOpp) {
        state.opponentActivePokemon.isParalyzed = true;
    }
    if (skill.specialEffect.poisonOpp) {
        state.opponentActivePokemon.isPoisoned = true;
    }
    if (skill.specialEffect.heal > 0) {
        state.activePokemon.hp += skill.specialEffect.heal;
    }
}

std::vector<GameState> generateNextStates(const GameState &state) {
    std::vector<GameState> nextStates;

    const Pokemon &attacker = state.activePokemon;
    nextStates.reserve(attacker.skills.size() + state.bench.size());

    for (const Skill &skill : attacker.skills) {
        GameState nextState = state;
        applySkillToState(nextState, skill);
        nextStates.push_back(std::move(nextState));
    }

    for (size_t i = 0; i < state.bench.size(); ++i) {
        GameState nextState = state;
        std::swap(nextState.activePokemon, nextState.bench[i]);
        nextStates.push_back(std::move(nextState));
    }

    return nextStates;
}

size_t pickDeckIndex(size_t deckSize, int simulationIndex) {
    std::mt19937 generator(0xC0FFEEu + static_cast<unsigned int>(simulationIndex));
    std::uniform_int_distribution<size_t> pickCard(0, deckSize - 1);
    return pickCard(generator);
}

Pokemon findCardOrFallback(
    const std::unordered_map<std::string, Pokemon> &cardMap,
    std::initializer_list<const char *> preferredNames
) {
    for (const char *preferredName : preferredNames) {
        auto it = cardMap.find(normalize(preferredName));
        if (it != cardMap.end()) {
            return it->second;
        }
    }

    if (!cardMap.empty()) {
        return cardMap.begin()->second;
    }

    return Pokemon();
}

void removeFirstCardByName(std::vector<Pokemon> &cards, const std::string &name) {
    auto it = std::find_if(cards.begin(), cards.end(),
                           [&name](const Pokemon &card) {
                               return normalize(card.name) == normalize(name);
                           });
    if (it != cards.end()) {
        cards.erase(it);
    }
}

void addAttachment(std::vector<EnergyAttachment> &attachments,
                   Pokemon &pokemon,
                   const std::string &energyType,
                   int amount) {
    pokemon.attachedEnergy.emplace_back(energyType, amount);
    attachments.push_back({pokemon.name, energyType, amount});
}

GameState buildBenchmarkState(const std::unordered_map<std::string, Pokemon> &cardMap,
                              const std::vector<Pokemon> &deckTemplate) {
    GameState state;
    state.deck = deckTemplate;
    state.turn = 4;
    state.firstTurn = false;

    state.activePokemon = findCardOrFallback(cardMap, {"Venusaur ex", "Venusaur", "Ivysaur", "Bulbasaur"});
    removeFirstCardByName(state.deck, state.activePokemon.name);

    std::vector<Pokemon> preferredBench = {
        findCardOrFallback(cardMap, {"Beedrill", "Ivysaur"}),
        findCardOrFallback(cardMap, {"Ivysaur", "Bulbasaur"}),
        findCardOrFallback(cardMap, {"Bulbasaur", "Weedle"})
    };
    for (const auto &pokemon : preferredBench) {
        if (!pokemon.name.empty()) {
            state.bench.push_back(pokemon);
            removeFirstCardByName(state.deck, pokemon.name);
        }
    }

    std::vector<Pokemon> preferredHand = {
        findCardOrFallback(cardMap, {"Poke Ball"}),
        findCardOrFallback(cardMap, {"Professor's Research"}),
        findCardOrFallback(cardMap, {"Potion"}),
        findCardOrFallback(cardMap, {"Sabrina"}),
        findCardOrFallback(cardMap, {"X Speed"})
    };
    for (const auto &card : preferredHand) {
        if (!card.name.empty()) {
            state.hand.push_back(card);
            removeFirstCardByName(state.deck, card.name);
        }
    }

    state.opponentActivePokemon = findCardOrFallback(cardMap, {"Mewtwo ex", "Dragonite", "Articuno ex"});
    state.opponentBench.push_back(findCardOrFallback(cardMap, {"Scyther", "Jynx", "Tentacool"}));
    state.opponentBench.push_back(findCardOrFallback(cardMap, {"Gastly", "Dratini", "Shellder"}));
    state.opponentBench.push_back(findCardOrFallback(cardMap, {"Haunter", "Pidgey", "Krabby"}));

    addAttachment(state.yourAttachments, state.activePokemon, state.activePokemon.type.empty() ? "Grass" : state.activePokemon.type, 2);
    addAttachment(state.yourAttachments, state.activePokemon, "Colorless", 2);

    if (!state.bench.empty()) {
        addAttachment(state.yourAttachments, state.bench[0], state.bench[0].type.empty() ? "Grass" : state.bench[0].type, 2);
    }
    if (state.bench.size() > 1) {
        addAttachment(state.yourAttachments, state.bench[1], state.bench[1].type.empty() ? "Grass" : state.bench[1].type, 1);
    }
    if (state.bench.size() > 2) {
        addAttachment(state.yourAttachments, state.bench[2], "Colorless", 1);
    }

    addAttachment(state.oppAttachments, state.opponentActivePokemon,
                  state.opponentActivePokemon.type.empty() ? "Psychic" : state.opponentActivePokemon.type, 3);
    for (size_t i = 0; i < state.opponentBench.size(); ++i) {
        if (!state.opponentBench[i].name.empty()) {
            addAttachment(state.oppAttachments, state.opponentBench[i],
                          state.opponentBench[i].type.empty() ? "Colorless" : state.opponentBench[i].type,
                          i == 0 ? 2 : 1);
        }
    }

    return state;
}

std::vector<int> buildThreadCounts(int maxThreads) {
    std::vector<int> threadCounts;
    for (int threads = 1; threads <= maxThreads; threads *= 2) {
        threadCounts.push_back(threads);
    }
    if (threadCounts.empty() || threadCounts.back() != maxThreads) {
        threadCounts.push_back(maxThreads);
    }
    return threadCounts;
}

template <typename Work>
double measureAverageSeconds(const Work &work, int repetitions, double &resultValue) {
    double totalSeconds = 0.0;
    resultValue = 0.0;

    for (int repeat = 0; repeat < repetitions; ++repeat) {
        const auto start = std::chrono::steady_clock::now();
        resultValue = work();
        const auto end = std::chrono::steady_clock::now();
        totalSeconds += std::chrono::duration<double>(end - start).count();
    }

    return totalSeconds / static_cast<double>(repetitions);
}

void printBenchmarkHeader(const std::string &title) {
    std::cout << "\n" << title << std::endl;
    std::cout << std::left
              << std::setw(10) << "Threads"
              << std::setw(14) << "Time (s)"
              << std::setw(12) << "Speedup"
              << std::setw(12) << "Efficiency"
              << std::setw(14) << "Score"
              << std::endl;
}

void printBenchmarkRow(int threads,
                       double runtimeSeconds,
                       double speedup,
                       double efficiency,
                       double score) {
    std::cout << std::left << std::fixed << std::setprecision(4)
              << std::setw(10) << threads
              << std::setw(14) << runtimeSeconds
              << std::setw(12) << speedup
              << std::setw(12) << efficiency
              << std::setw(14) << score
              << std::endl;
}

}

void loadAllMetaDecks(const std::string &filename) {
    allMetaDecks.clear();
    std::ifstream metaFile(filename);
    if (!metaFile.is_open()) {
        std::cerr << "Error: Could not open meta-decks file: " << filename << std::endl;
        return;
    }

    std::string line;
    std::string currentDeck;
    while (std::getline(metaFile, line)) {
        line = trim(line);
        if (line == "BEGIN_DECK") {
            currentDeck.clear();
        } else if (line == "END_DECK") {
            allMetaDecks.push_back(currentDeck);
        } else {
            currentDeck += line + "\n";
        }
    }
    metaFile.close();
}

std::vector<std::string> filterMetaDecksByVisibleBoard(
    const std::vector<std::string> &visiblePokemons
) {
    std::vector<std::string> filteredDecks;

    #pragma omp parallel
    {
        std::vector<std::string> localFilteredDecks;

        #pragma omp for schedule(dynamic)
        for (size_t i = 0; i < allMetaDecks.size(); ++i) {
            const auto &deck = allMetaDecks[i];
            std::istringstream deckStream(deck);
            std::unordered_set<std::string> deckPokemons;
            std::string line;

            while (std::getline(deckStream, line)) {
                line = trim(line);
                if (line.empty() || line == "BEGIN_DECK" || line == "END_DECK") continue;

                auto tokens = splitAndTrim(line, ',');
                if (tokens.size() == 2) {
                    deckPokemons.insert(toLower(tokens[0]));
                }
            }

            bool allMatch = true;
            for (const auto &visiblePoke : visiblePokemons) {
                if (deckPokemons.find(toLower(visiblePoke)) == deckPokemons.end()) {
                    allMatch = false;
                    break;
                }
            }

            if (allMatch) {
                localFilteredDecks.push_back(deck);
            }
        }
        #pragma omp critical
        filteredDecks.insert(filteredDecks.end(), localFilteredDecks.begin(), localFilteredDecks.end());
    }

    if (filteredDecks.empty()) {
        return allMetaDecks;
    }

    return filteredDecks;
}

void updateMetaDeckGuesses(GameState &state) {
    std::vector<std::string> visiblePokemons;

    if (!state.opponentActivePokemon.name.empty()) {
        visiblePokemons.push_back(state.opponentActivePokemon.name);
    }
    for (const auto &benchPoke : state.opponentBench) {
        if (!benchPoke.name.empty()) {
            visiblePokemons.push_back(benchPoke.name);
        }
    }

    state.oppMetaDeckGuesses = filterMetaDecksByVisibleBoard(visiblePokemons);
}

double evaluateGameState(const GameState &state, int depth) {
    if (!state.opponentActivePokemon.name.empty() && state.opponentActivePokemon.hp <= 0) {
        return 1.0;
    }
    if (!state.activePokemon.name.empty() && state.activePokemon.hp <= 0 && state.bench.empty()) {
        return 0.0;
    }

    double score = 0.0;

    score += scorePokemon(state.activePokemon, true);
    for (const auto &pokemon : state.bench) {
        score += scorePokemon(pokemon, false);
    }
    score += 4.0 * static_cast<double>(state.hand.size());
    score += 2.0 * static_cast<double>(state.deck.size());
    score += 3.0 * static_cast<double>(state.yourAttachments.size());

    score -= scorePokemon(state.opponentActivePokemon, true);
    for (const auto &pokemon : state.opponentBench) {
        score -= scorePokemon(pokemon, false);
    }
    score -= 3.0 * static_cast<double>(state.oppAttachments.size());

    score += 2.5 * static_cast<double>(depth);

    return clampProbability(0.5 + (score / 400.0));
}

double monteCarloSimulation(const GameState &state, int numSimulations) {
    if (numSimulations <= 0) {
        return evaluateGameState(state);
    }

    double totalOutcome = 0.0;

    #pragma omp parallel for reduction(+:totalOutcome) schedule(dynamic)
    for (int i = 0; i < numSimulations; ++i) {
        GameState sampledState = state;

        if (!sampledState.deck.empty() && sampledState.hand.size() < MAX_HAND_SIZE) {
            const size_t cardIndex = pickDeckIndex(sampledState.deck.size(), i);
            sampledState.hand.push_back(sampledState.deck[cardIndex]);
            sampledState.deck.erase(sampledState.deck.begin() + cardIndex);
        }

        totalOutcome += evaluateGameState(sampledState);
    }

    return totalOutcome / static_cast<double>(numSimulations);
}

double monteCarloSimulationSequential(const GameState &state, int numSimulations) {
    if (numSimulations <= 0) {
        return evaluateGameState(state);
    }

    double totalOutcome = 0.0;

    for (int i = 0; i < numSimulations; ++i) {
        GameState sampledState = state;

        if (!sampledState.deck.empty() && sampledState.hand.size() < MAX_HAND_SIZE) {
            const size_t cardIndex = pickDeckIndex(sampledState.deck.size(), i);
            sampledState.hand.push_back(sampledState.deck[cardIndex]);
            sampledState.deck.erase(sampledState.deck.begin() + cardIndex);
        }

        totalOutcome += evaluateGameState(sampledState);
    }

    return totalOutcome / static_cast<double>(numSimulations);
}

void processRoundInput(GameState &state) {
    std::string input;
    std::cout << "\nEnter round information (type 'exit' to terminate):" << std::endl;

    while (true) {
        std::cout << "Do you want to place a Pokémon from your hand to the bench? (yes/no, or press 0 to go back): ";
        std::getline(std::cin, input);
        input = toLower(input);
        if (input == "exit") {
            std::cout << "Exiting the program. Goodbye!" << std::endl;
            exit(0);
        }
        if (input == "yes" || input == "no" || input == "0") break;
        std::cerr << "Error: Invalid input. Please enter 'yes', 'no', or press 0 to go back." << std::endl;
    }
    if (input == "0") return;

    if (input == "yes") {
        if (state.hand.empty()) {
            std::cout << "Your hand is empty. No Pokémon to place on the bench." << std::endl;
        } else if (state.bench.size() >= MAX_BENCH) {
            std::cout << "Your bench is full. Cannot place more Pokémon." << std::endl;
        } else {
            std::vector<int> basicPokemonIndices;
            std::cout << "Basic Pokémon in your hand:" << std::endl;
            for (size_t i = 0; i < state.hand.size(); ++i) {
                if (state.hand[i].stage == 0) {
                    basicPokemonIndices.push_back(i);
                    std::cout << "  " << basicPokemonIndices.size() << ". " << state.hand[i].name << std::endl;
                }
            }

            if (basicPokemonIndices.empty()) {
                std::cout << "No basic Pokémon available in your hand to place on the bench." << std::endl;
            } else {
                int choice;
                while (true) {
                    std::cout << "Select the Pokémon to place on the bench (or press 0 to go back): ";
                    std::getline(std::cin, input);
                    if (toLower(input) == "exit") {
                        std::cout << "Exiting the program. Goodbye!" << std::endl;
                        exit(0);
                    }
                    try {
                        choice = std::stoi(input);
                        if (choice == 0) return;
                        if (choice >= 1 && choice <= static_cast<int>(basicPokemonIndices.size())) {
                            int selectedIndex = basicPokemonIndices[choice - 1];
                            state.bench.push_back(state.hand[selectedIndex]);
                            state.hand.erase(state.hand.begin() + selectedIndex);
                            std::cout << "Placed " << state.bench.back().name << " on the bench." << std::endl;
                            break;
                        }
                    } catch (...) {
                    }
                    std::cerr << "Error: Invalid choice. Please select a valid Pokémon index or press 0 to go back." << std::endl;
                }
            }
        }
    }

    while (true) {
        std::cout << "Do you want to evolve a Pokémon? (yes/no, or press 0 to go back): ";
        std::getline(std::cin, input);
        input = toLower(input);
        if (input == "exit") {
            std::cout << "Exiting the program. Goodbye!" << std::endl;
            exit(0);
        }
        if (input == "yes" || input == "no" || input == "0") break;
        std::cerr << "Error: Invalid input. Please enter 'yes', 'no', or press 0 to go back." << std::endl;
    }
    if (input == "0") return;

    if (input == "yes") {
        bool evolutionAvailable = false;
        for (auto &benchPoke : state.bench) {
            if (benchPoke.postEvolution != nullptr) {
                auto it = std::find_if(state.hand.begin(), state.hand.end(),
                                       [&benchPoke](const Pokemon &card) {
                                           return card.name == benchPoke.postEvolution->name;
                                       });
                if (it != state.hand.end()) {
                    evolutionAvailable = true;
                    break;
                }
            }
        }
        if (!evolutionAvailable) {
            std::cout << "No valid evolution cards available in your hand." << std::endl;
        } else {
            std::cout << "Pokémon on your bench:" << std::endl;
            for (size_t i = 0; i < state.bench.size(); ++i) {
                std::cout << "  " << (i + 1) << ". " << state.bench[i].name
                          << (state.bench[i].postEvolution == nullptr ? " (No evolution available)" : "") << std::endl;
            }
            int choice;
            while (true) {
                std::cout << "Select the Pokémon to evolve (or press 0 to go back): ";
                std::getline(std::cin, input);
                if (toLower(input) == "exit") {
                    std::cout << "Exiting the program. Goodbye!" << std::endl;
                    exit(0);
                }
                try {
                    choice = std::stoi(input);
                    if (choice == 0) return;
                    if (choice >= 1 && choice <= static_cast<int>(state.bench.size())) {
                        Pokemon &selectedPoke = state.bench[choice - 1];
                        if (selectedPoke.postEvolution == nullptr) {
                            std::cout << selectedPoke.name << " cannot evolve." << std::endl;
                        } else {
                            auto it = std::find_if(state.hand.begin(), state.hand.end(),
                                                   [&selectedPoke](const Pokemon &card) {
                                                       return card.name == selectedPoke.postEvolution->name;
                                                   });
                            if (it != state.hand.end()) {
                                selectedPoke = *it;
                                state.hand.erase(it);
                                std::cout << "Evolved to " << selectedPoke.name << "." << std::endl;
                                break;
                            } else {
                                std::cout << "Evolution card for " << selectedPoke.name
                                          << " is not available in your hand." << std::endl;
                            }
                        }
                    }
                } catch (...) {
                }
                std::cerr << "Error: Invalid choice. Please select a valid Pokémon index or press 0 to go back." << std::endl;
            }
        }
    }

    int action;
    while (true) {
        std::cout << "Select action (1 = Attack, 2 = Retreat, 3 = Use Item, 4 = Use Supporter, 5 = Nothing, or press 0 to go back): ";
        std::getline(std::cin, input);
        if (toLower(input) == "exit") {
            std::cout << "Exiting the program. Goodbye!" << std::endl;
            exit(0);
        }
        if (input == "0") return;
        try {
            action = std::stoi(input);
            if (action >= 1 && action <= 5) break;
        } catch (...) {
        }
        std::cerr << "Error: Invalid input. Please enter a number between 1 and 5, or press 0 to go back." << std::endl;
    }

    switch (action) {
    case 1: {
        std::cout << "You chose to Attack." << std::endl;
        auto &atkPoke = state.activePokemon;
        auto &defPoke = state.opponentActivePokemon;

        if (atkPoke.skills.empty()) {
            std::cout << "No skills available for the active Pokémon." << std::endl;
            break;
        }

        std::cout << "Available skills:" << std::endl;
        for (size_t i = 0; i < atkPoke.skills.size(); ++i) {
            const auto &skill = atkPoke.skills[i];
            std::cout << "  " << (i + 1) << ". " << skill.skillName 
                      << " (Damage: " << skill.dmg << ")" << std::endl;
        }

        int choice;
        while (true) {
            std::cout << "Choose skill number (or type 'exit' to terminate): ";
            std::getline(std::cin, input);
            if (toLower(input) == "exit") {
                std::cout << "Exiting the program. Goodbye!" << std::endl;
                exit(0);
            }
            try {
                choice = std::stoi(input);
                if (choice >= 1 && choice <= static_cast<int>(atkPoke.skills.size())) break;
            } catch (...) {
            }
            std::cerr << "Error: Invalid choice. Please select a valid skill number, or type 'exit' to terminate." << std::endl;
        }

        Skill &used = atkPoke.skills[choice - 1];
        std::cout << atkPoke.name << " uses " << used.skillName 
                  << ", dealing " << used.dmg << " damage to " 
                  << defPoke.name << "!" << std::endl;

        defPoke.hp -= used.dmg;
        if (defPoke.hp < 0) defPoke.hp = 0;

        std::cout << defPoke.name << " now has " 
                  << defPoke.hp << " HP remaining." << std::endl;

        if (used.specialEffect.paralyzeOpp && used.specialEffect.doCoinFlips) {
            char flip;
            while (true) {
                std::cout << "Flip a coin to attempt to paralyze " 
                          << defPoke.name << " (H=paralyze, T=no, or type 'exit' to terminate): ";
                std::getline(std::cin, input);
                if (toLower(input) == "exit") {
                    std::cout << "Exiting the program. Goodbye!" << std::endl;
                    exit(0);
                }
                if (input.size() == 1 && (input[0] == 'H' || input[0] == 'T')) {
                    flip = std::toupper(input[0]);
                    break;
                }
                std::cerr << "Error: Invalid input. Please enter 'H', 'T', or type 'exit' to terminate." << std::endl;
            }
            if (flip == 'H') {
                defPoke.isParalyzed = true;
                std::cout << defPoke.name << " is now Paralyzed!" << std::endl;
            } else {
                std::cout << "No paralysis effect." << std::endl;
            }
        }

        break;
    }

    case 2: {
        std::cout << "You chose to Retreat." << std::endl;
        if (state.bench.empty()) {
            std::cout << "No benched Pokémon available. Cannot retreat." << std::endl;
        } else {
            std::cout << "Benched Pokémon:" << std::endl;
            for (size_t i = 0; i < state.bench.size(); ++i) {
                std::cout << "  " << (i + 1) << ". " << state.bench[i].name << std::endl;
            }

            int index;
            while (true) {
                std::cout << "Select the index of the benched Pokémon to become active (or type 'exit' to terminate): ";
                std::getline(std::cin, input);
                if (toLower(input) == "exit") {
                    std::cout << "Exiting the program. Goodbye!" << std::endl;
                    exit(0);
                }
                try {
                    index = std::stoi(input);
                    if (index >= 1 && index <= static_cast<int>(state.bench.size())) break;
                } catch (...) {
                }
                std::cerr << "Error: Invalid index. Please select a valid benched Pokémon index, or type 'exit' to terminate." << std::endl;
            }

            std::swap(state.activePokemon, state.bench[index - 1]);
            std::cout << "Active Pokémon is now: " << state.activePokemon.name << std::endl;
        }

        while (true) {
            std::cout << "Did you make an attack after retreating? (yes/no, or type 'exit' to terminate): ";
            std::getline(std::cin, input);
            input = toLower(input);
            if (input == "exit") {
                std::cout << "Exiting the program. Goodbye!" << std::endl;
                exit(0);
            }
            if (input == "yes" || input == "no") break;
            std::cerr << "Error: Invalid input. Please enter 'yes', 'no', or type 'exit' to terminate." << std::endl;
        }
        if (input == "yes") {
            processRoundInput(state);
        }
        break;
    }

    case 3: {
        std::cout << "You chose to use an Item card." << std::endl;
        std::cout << "Enter the name of the Item card you want to use (or type 'exit' to terminate): ";
        std::getline(std::cin, input);
        if (toLower(input) == "exit") {
            std::cout << "Exiting the program. Goodbye!" << std::endl;
            exit(0);
        }

        auto it = std::find_if(state.hand.begin(), state.hand.end(),
                               [&input](const Pokemon &card) { return toLower(card.name) == toLower(input); });

        if (it != state.hand.end()) {
            std::cout << "Using Item card: " << it->name << std::endl;

            if (toLower(it->name) == "poke ball") {
                std::cout << "What card did you draw with Poke Ball? ";
                std::string drawnCard;
                std::getline(std::cin, drawnCard);
                if (!drawnCard.empty()) {
                    if (state.hand.size() < MAX_HAND_SIZE) {
                        auto cardIt = std::find_if(state.deck.begin(), state.deck.end(),
                                                   [&drawnCard](const Pokemon &card) {
                                                       return normalize(card.name) == normalize(drawnCard);
                                                   });
                        if (cardIt != state.deck.end()) {
                            state.hand.push_back(*cardIt);
                            state.deck.erase(cardIt);
                            std::cout << "Added " << drawnCard << " to your hand." << std::endl;
                        } else {
                            std::cerr << "Error: Card '" << drawnCard << "' not found in your deck." << std::endl;
                        }
                    } else {
                        std::cerr << "Error: Hand size limit of " << MAX_HAND_SIZE
                                  << " cards reached. Cannot add more cards." << std::endl;
                    }
                }
            }

            state.hand.erase(it);
        } else {
            std::cerr << "Error: Item card '" << input << "' not found in your hand." << std::endl;
        }

        while (true) {
            std::cout << "Did you make an attack after using the Item card? (yes/no, or type 'exit' to terminate): ";
            std::getline(std::cin, input);
            input = toLower(input);
            if (input == "exit") {
                std::cout << "Exiting the program. Goodbye!" << std::endl;
                exit(0);
            }
            if (input == "yes" || input == "no") break;
            std::cerr << "Error: Invalid input. Please enter 'yes', 'no', or type 'exit' to terminate." << std::endl;
        }
        if (input == "yes") {
            processRoundInput(state);
        }
        break;
    }

    case 4: {
        std::cout << "You chose to use a Supporter card." << std::endl;
        std::cout << "Enter the name of the Supporter card you want to use (or type 'exit' to terminate): ";
        std::getline(std::cin, input);
        if (toLower(input) == "exit") {
            std::cout << "Exiting the program. Goodbye!" << std::endl;
            exit(0);
        }

        auto it = std::find_if(state.hand.begin(), state.hand.end(),
                               [&input](const Pokemon &card) { return toLower(card.name) == toLower(input); });

        if (it != state.hand.end()) {
            std::cout << "Using Supporter card: " << it->name << std::endl;
            state.hand.erase(it);
        } else {
            std::cerr << "Error: Supporter card '" << input << "' not found in your hand." << std::endl;
        }

        while (true) {
            std::cout << "Did you make an attack after using the Supporter card? (yes/no, or type 'exit' to terminate): ";
            std::getline(std::cin, input);
            input = toLower(input);
            if (input == "exit") {
                std::cout << "Exiting the program. Goodbye!" << std::endl;
                exit(0);
            }
            if (input == "yes" || input == "no") break;
            std::cerr << "Error: Invalid input. Please enter 'yes', 'no', or type 'exit' to terminate." << std::endl;
        }
        if (input == "yes") {
            processRoundInput(state);
        }
        break;
    }

    case 5: {
        std::cout << "You chose to do Nothing. Skipping turn." << std::endl;
        break;
    }

    default:
        std::cout << "Skipped turn." << std::endl;
        break;
    }
}

void loadPresetDeck(
    const std::string &deckFile,
    const std::unordered_map<std::string, Pokemon> &cardMap,
    std::vector<Pokemon> &deck
) {
    deck.clear();
    std::ifstream file(deckFile);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open deck file: " << deckFile << std::endl;
        return;
    }

    std::unordered_map<std::string, int> copyCounts;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty()) continue;

        auto tokens = splitAndTrim(line, ',');
        if (tokens.size() < 2) {
            std::cerr << "Warning: Deck entry malformed (need name, count): '"
                      << line << "'" << std::endl;
            continue;
        }

        const std::string cardName = tokens[0];
        std::string countStr = trim(tokens[1]);

        bool allDigits = !countStr.empty();
        for (char c : countStr) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                allDigits = false;
                break;
            }
        }
        if (!allDigits) {
            std::cerr << "Warning: Invalid count for '" << cardName
                      << "': '" << countStr << "' — skipping." << std::endl;
            continue;
        }

        int count = std::stoi(countStr);
        if (count > MAX_CARD_COPIES) {
            std::cerr << "Warning: '" << cardName << "' exceeds the " << MAX_CARD_COPIES
                      << "-copy limit. Truncating to " << MAX_CARD_COPIES << "." << std::endl;
            count = MAX_CARD_COPIES;
        }

        auto it = cardMap.find(normalize(cardName));
        if (it == cardMap.end()) {
            std::cerr << "Warning: Card '" << cardName
                      << "' not found in card database." << std::endl;
            continue;
        }

        for (int i = 0; i < count; ++i) {
            const std::string normalizedName = normalize(cardName);
            if (deck.size() >= DECK_SIZE) {
                std::cerr << "Warning: Deck already has " << DECK_SIZE
                          << " cards. Extra cards are ignored." << std::endl;
                break;
            }

            if (copyCounts[normalizedName] >= MAX_CARD_COPIES) {
                std::cerr << "Warning: Skipping extra copy of '" << cardName
                          << "' beyond the allowed limit." << std::endl;
                break;
            }

            deck.push_back(it->second);
            copyCounts[normalizedName]++;
        }
    }

    file.close();

    if (deck.size() != DECK_SIZE) {
        std::cerr << "Warning: Loaded deck has " << deck.size()
                  << " cards; expected " << DECK_SIZE << "." << std::endl;
    }
}

void validateDeck(const std::vector<Pokemon> &deck) {
    std::unordered_map<std::string, int> cardCounts;

    for (size_t i = 0; i < deck.size(); ++i) {
        const Pokemon &card = deck[i];
        if (card.name.empty()) {
            std::cerr << "Warning: Card at index " << i << " has no name." << std::endl;
            continue;
        }

        const std::string normalizedName = normalize(card.name);
        cardCounts[normalizedName]++;
        if (cardCounts[normalizedName] > MAX_CARD_COPIES) {
            std::cerr << "Warning: Deck contains more than " << MAX_CARD_COPIES
                      << " copies of '" << card.name << "'." << std::endl;
        }
    }

    if (deck.size() != DECK_SIZE) {
        std::cerr << "Warning: Deck contains " << deck.size()
                  << " cards; expected " << DECK_SIZE << "." << std::endl;
    }
}

void drawInitialHand(GameState &state) {
    std::cout << "\nEnter your initial hand (5 cards, separated by commas): ";
    std::string input;
    std::getline(std::cin, input);

    auto cardNames = splitAndTrim(input, ',');
    if (cardNames.size() != INITIAL_HAND_SIZE) {
        std::cerr << "Error: You must select exactly " << INITIAL_HAND_SIZE << " cards for your initial hand." << std::endl;
        return;
    }

    for (const auto &cardName : cardNames) {
        auto it = std::find_if(state.deck.begin(), state.deck.end(),
                               [&cardName](const Pokemon &card) {
                                   return normalize(card.name) == normalize(cardName);
                               });
        if (it != state.deck.end()) {
            if (state.hand.size() < MAX_HAND_SIZE) {
                state.hand.push_back(*it);
                state.deck.erase(it);
            } else {
                std::cerr << "Error: Hand size limit of " << MAX_HAND_SIZE
                          << " cards reached. Cannot add more cards." << std::endl;
                break;
            }
        } else {
            std::cerr << "Error: Card '" << cardName << "' is not in your deck." << std::endl;
            return;
        }
    }

}

double simulateDecisionTree(const GameState &state, int depth) {
    if (depth == 0) {
        return evaluateGameState(state);
    }

    std::vector<GameState> nextStates = generateNextStates(state);

    if (nextStates.empty()) {
        return evaluateGameState(state);
    }

    double totalOutcome = 0.0;

    #pragma omp parallel for reduction(+:totalOutcome) schedule(dynamic)
    for (size_t i = 0; i < nextStates.size(); ++i) {
        totalOutcome += simulateDecisionTreeSequential(nextStates[i], depth - 1);
    }

    return totalOutcome / nextStates.size();
}

double simulateDecisionTreeSequential(const GameState &state, int depth) {
    if (depth == 0) {
        return evaluateGameState(state);
    }

    std::vector<GameState> nextStates = generateNextStates(state);

    if (nextStates.empty()) {
        return evaluateGameState(state);
    }

    double totalOutcome = 0.0;

    for (const auto &nextState : nextStates) {
        totalOutcome += simulateDecisionTreeSequential(nextState, depth - 1);
    }

    return totalOutcome / nextStates.size();
}

void preStartConfiguration(const GameState &state) {
    std::cout << "Pre-Start: Current Deck Composition:" << std::endl;
    for (const auto &card : state.deck) {
        std::cout << "  " << card.name << std::endl;
    }
}

void preFirstRoundConfiguration(GameState &state) {
    std::string coinResult;
    std::cout << "\nPre-1st Round Configuration:" << std::endl;
    std::cout << "Enter coin flip result (H for Heads, T for Tails): ";
    std::cin >> coinResult;
    if (toLower(coinResult) == "h") {
        std::cout << "You will go first." << std::endl;
    } else {
        std::cout << "You will go second." << std::endl;
    }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    std::string opponentEnergy;
    std::cout << "Enter opponent's main energy type: ";
    std::getline(std::cin, opponentEnergy);
    std::cout << "Opponent's main energy type: " << opponentEnergy << std::endl;

    std::cout << "Your Initial Hand:" << std::endl;
    for (const auto &card : state.hand) {
        std::cout << "  " << card.name << std::endl;
    }
}

void postFirstRoundUpdate(GameState &state, const std::unordered_map<std::string, Pokemon> &cardMap) {
    std::cout << "\nPost-1st Round Update:\n";

    std::cout << "Enter your Active Pokémon (blank to keep current): ";
    std::string s;
    std::getline(std::cin, s);
    if (!s.empty()) {
        auto it = cardMap.find(normalize(s));
        if (it != cardMap.end()) {
            state.activePokemon = it->second;
        } else {
            std::cerr << "Error: Pokémon '" << s << "' not found in card database.\n";
        }
    }

    std::cout << "Enter your Benched Pokémon (comma-separated): ";
    std::getline(std::cin, s);
    state.bench.clear();
    for (auto &name : splitAndTrim(s, ',')) {
        auto it = cardMap.find(normalize(name));
        if (it != cardMap.end()) {
            state.bench.push_back(it->second);
        } else {
            std::cerr << "Error: Pokémon '" << name << "' not found in card database.\n";
        }
    }

    std::cout << "Enter your Energy attachments (format: PokémonName:EnergyType:Amount, …): ";
    std::getline(std::cin, s);
    state.yourAttachments.clear();

    auto tokens = splitAndTrim(s, ',');
    for (const auto &token : tokens) {
        auto parts = splitAndTrim(token, ':');
        if (parts.size() == 3) {
            state.yourAttachments.push_back({
                parts[0],
                parts[1],
                parseIntOrZero(parts[2])
            });
        } else if (parts.size() == 2) {
            auto subParts = splitAndTrim(parts[1], ',');
            if (subParts.size() == 2) {
                state.yourAttachments.push_back({
                    parts[0],
                    subParts[0],
                    parseIntOrZero(subParts[1])
                });
            } else {
                std::cerr << "Invalid energy attachment format: " << token << std::endl;
            }
        } else {
            std::cerr << "Invalid energy attachment format: " << token << std::endl;
        }
    }

    std::cout << "Enter opponent’s Active Pokémon: ";
    std::getline(std::cin, s);
    if (!s.empty()) {
        auto it = cardMap.find(normalize(s));
        if (it != cardMap.end()) {
            state.opponentActivePokemon = it->second;
        } else {
            std::cerr << "Error: Pokémon '" << s << "' not found in card database.\n";
        }
    }

    std::cout << "Enter opponent’s Benched Pokémon (comma-separated): ";
    std::getline(std::cin, s);
    state.opponentBench.clear();
    for (auto &name : splitAndTrim(s, ',')) {
        auto it = cardMap.find(normalize(name));
        if (it != cardMap.end()) {
            state.opponentBench.push_back(it->second);
        } else {
            std::cerr << "Error: Pokémon '" << name << "' not found in card database.\n";
        }
    }

    std::cout << "Enter opponent’s Energy attachments (format: Pokémon:EnergyType:Count, …): ";
    std::getline(std::cin, s);
    state.oppAttachments.clear();
    for (auto &tok : splitAndTrim(s, ',')) {
        auto parts = splitAndTrim(tok, ':');
        if (parts.size() == 3) {
            state.oppAttachments.push_back({
                parts[0],
                parts[1],
                parseIntOrZero(parts[2])
            });
        } else {
            std::cerr << "Invalid energy attachment format: " << tok << std::endl;
        }
    }

    std::cout << "How many attacks were used this round? ";
    int n;
    std::cin >> n;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    state.attacksThisRound.clear();
    for (int i = 0; i < n; ++i) {
        AttackRecord rec;
        std::cout << "Attacker name: ";
        std::getline(std::cin, rec.attacker);
        std::cout << "Move used: ";
        std::getline(std::cin, rec.moveName);
        std::cout << "Target: ";
        std::getline(std::cin, rec.target);
        std::cout << "Effects (semicolon-separated): ";
        std::string eff;
        std::getline(std::cin, eff);
        rec.effects = splitAndTrim(eff, ';');
        state.attacksThisRound.push_back(rec);
    }

    std::unordered_set<std::string> oppEnergies;
    for (auto &ea : state.oppAttachments) {
        oppEnergies.insert(ea.energyType);
    }

    std::unordered_set<std::string> oppPokemons;
    oppPokemons.insert(state.opponentActivePokemon.name);
    for (auto &p : state.opponentBench) {
        oppPokemons.insert(p.name);
    }

    (void)oppEnergies;
    (void)oppPokemons;
    updateMetaDeckGuesses(state);
}

void preEveryRoundConfiguration(GameState &state) {
    std::cout << "\nPre-Every Round Configuration:\n";

    std::cout << "Enter the name of the drawn card: ";
    std::string drawnCard;
    std::getline(std::cin, drawnCard);

    if (!drawnCard.empty() && toLower(drawnCard) != "none") {
        if (state.hand.size() < MAX_HAND_SIZE) {
            std::cout << "Card '" << drawnCard << "' drawn.\n";
            state.hand.push_back(Pokemon(drawnCard));
        } else {
            std::cerr << "Error: Hand size limit of " << MAX_HAND_SIZE
                      << " cards reached. Cannot add more cards." << std::endl;
        }
    }
}

void postEveryRoundUpdate(GameState &state) {
    std::cout << "\nPost-Every Round Update:\n";

    int action;
    std::string input;
    while (true) {
        std::cout << "Opponent's action (1 = Attack, 2 = Retreat, 3 = Use Item, 4 = Use Supporter, 5 = Nothing, or press 0 to go back): ";
        std::getline(std::cin, input);
        if (toLower(input) == "exit") {
            std::cout << "Exiting the program. Goodbye!" << std::endl;
            exit(0);
        }
        if (input == "0") return;
        try {
            action = std::stoi(input);
            if (action >= 1 && action <= 5) break;
        } catch (...) {
        }
        std::cerr << "Error: Invalid input. Please enter a number between 1 and 5, or press 0 to go back." << std::endl;
    }

    switch (action) {
    case 1: {
        const auto &oppActive = state.opponentActivePokemon;
        if (oppActive.skills.empty()) {
            std::cout << "Opponent's active Pokémon (" << oppActive.name << ") has no available skills.\n";
        } else {
            std::cout << "Opponent's active Pokémon (" << oppActive.name << ") available skills:\n";
            for (size_t i = 0; i < oppActive.skills.size(); ++i) {
                const auto &skill = oppActive.skills[i];
                std::cout << "  " << (i + 1) << ". " << skill.skillName 
                          << " (Damage: " << skill.dmg << ")\n";
            }
        }

        std::cout << "Choose the skill number used by the opponent: ";
        int choice;
        if (!(std::cin >> choice) || choice < 1 || choice > static_cast<int>(oppActive.skills.size())) {
            std::cerr << "Invalid choice. Attack canceled.\n";
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return;
        }
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        const Skill &usedSkill = oppActive.skills[choice - 1];
        std::cout << oppActive.name << " uses " << usedSkill.skillName 
                  << ", dealing " << usedSkill.dmg << " damage to " 
                  << state.activePokemon.name << "!\n";

        state.activePokemon.hp -= usedSkill.dmg;
        if (state.activePokemon.hp < 0) state.activePokemon.hp = 0;

        std::cout << state.activePokemon.name << " now has " 
                  << state.activePokemon.hp << " HP remaining.\n";

        if (usedSkill.specialEffect.paralyzeOpp && usedSkill.specialEffect.doCoinFlips) {
            std::cout << "Flip a coin to attempt to paralyze " 
                      << state.activePokemon.name << " (H=paralyze, T=no): ";
            char flip;
            std::cin >> flip;
            if (std::toupper(flip) == 'H') {
                state.activePokemon.isParalyzed = true;
                std::cout << state.activePokemon.name << " is now Paralyzed!\n";
            } else {
                std::cout << "No paralysis effect.\n";
            }
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }
        break;
    }

    case 2: {
        std::cout << "Opponent chose to Retreat.\n";
        break;
    }

    case 3: {
        std::cout << "Opponent used an Item card." << std::endl;
        std::cout << "Enter the name of the Item card the opponent used: ";
        std::string itemName;
        std::getline(std::cin, itemName);

        if (toLower(itemName) == "poke ball") {
            std::cout << "Opponent used Poke Ball. Assume they drew a basic Pokémon." << std::endl;
            state.opponentBench.push_back(Pokemon("Unknown Basic Pokémon", false, "", "", true, 0, 0, 0));
            std::cout << "Added a placeholder for a basic Pokémon to the opponent's hand." << std::endl;
        }

        break;
    }

    case 4: {
        std::cout << "Opponent used a Supporter card.\n";
        break;
    }

    case 5: {
        std::cout << "Opponent chose to do Nothing.\n";
        break;
    }

    default:
        std::cerr << "Error: Invalid action. No changes made.\n";
        break;
    }
}

void updateGameState(GameState &state) {
    std::cout << "\nUpdate Your Board State:" << std::endl;

    std::cout << "Enter Active Pokémon: ";
    std::string activeName;
    std::getline(std::cin, activeName);
    auto activeIt = std::find_if(state.deck.begin(), state.deck.end(),
                                 [&activeName](const Pokemon &card) { return card.name == activeName; });
    if (activeIt != state.deck.end()) {
        state.activePokemon = *activeIt;
    } else {
        std::cerr << "Error: Active Pokémon '" << activeName << "' not found in your deck." << std::endl;
        return;
    }

    std::cout << "Enter Benched Pokémon (comma-separated): ";
    std::string benchInput;
    std::getline(std::cin, benchInput);
    auto benchNames = splitAndTrim(benchInput, ',');
    state.bench.clear();
    for (const auto &name : benchNames) {
        auto benchIt = std::find_if(state.deck.begin(), state.deck.end(),
                                    [&name](const Pokemon &card) { return card.name == name; });
        if (benchIt != state.deck.end()) {
            state.bench.push_back(*benchIt);
        } else {
            std::cerr << "Error: Benched Pokémon '" << name << "' not found in your deck." << std::endl;
        }
    }

    std::cout << "Enter Energy Attachments (format: PokémonName:EnergyType:Amount, ...): ";
    std::string energyInput;
    std::getline(std::cin, energyInput);
    auto energyTokens = splitAndTrim(energyInput, ',');

    for (const auto &token : energyTokens) {
        auto parts = splitAndTrim(token, ':');
        if (parts.size() != 3) {
            std::cerr << "Invalid energy attachment format: " << token << std::endl;
            continue;
        }

        const std::string &pokemonName = parts[0];
        const std::string &energyType = parts[1];
        int amount = parseIntOrZero(parts[2]);

        auto benchIt = std::find_if(state.bench.begin(), state.bench.end(),
                                    [&pokemonName](const Pokemon &card) { return card.name == pokemonName; });
        if (benchIt != state.bench.end()) {
            benchIt->attachedEnergy.push_back(EnergyRequirement(energyType, amount));
        } else if (state.activePokemon.name == pokemonName) {
            state.activePokemon.attachedEnergy.push_back(EnergyRequirement(energyType, amount));
        } else {
            std::cerr << "Error: Pokémon '" << pokemonName << "' not found on your board." << std::endl;
        }
    }
}

void printActionProbabilities(const GameState &state, int simulationDepth) {
    std::cout << "\n--- Action Probabilities ---\n";

    if (!state.activePokemon.skills.empty()) {
        std::cout << "Active Pokémon (" << state.activePokemon.name << ") attacking:\n";
        for (size_t i = 0; i < state.activePokemon.skills.size(); ++i) {
            GameState simulatedState = state;
            const Skill &skill = state.activePokemon.skills[i];
            simulatedState.opponentActivePokemon.hp -= skill.dmg;
            if (simulatedState.opponentActivePokemon.hp < 0) {
                simulatedState.opponentActivePokemon.hp = 0;
            }
            double probability = simulateDecisionTree(simulatedState, simulationDepth) * 100;
            std::cout << "  Skill " << (i + 1) << " (" << skill.skillName << "): " 
                      << probability << "% chance of winning\n";
        }
    }

    for (size_t i = 0; i < state.bench.size(); ++i) {
        GameState simulatedState = state;
        std::swap(simulatedState.activePokemon, simulatedState.bench[i]);
        double probability = simulateDecisionTree(simulatedState, simulationDepth) * 100;
        std::cout << "Switching to benched Pokémon " << (i + 1) << " (" 
                  << simulatedState.activePokemon.name << "): " 
                  << probability << "% chance of winning\n";
    }

    for (const auto &card : state.hand) {
        if (card.cardType == 1 || card.cardType == 2) {
            GameState simulatedState = state;
            simulatedState.hand.erase(
                std::remove_if(simulatedState.hand.begin(), simulatedState.hand.end(),
                               [&card](const Pokemon &c) { return c.name == card.name; }),
                simulatedState.hand.end());
            double probability = simulateDecisionTree(simulatedState, simulationDepth) * 100;
            std::cout << "Using " << (card.cardType == 1 ? "Supporter" : "Item") 
                      << " card (" << card.name << "): " 
                      << probability << "% chance of winning\n";
        }
    }
}

void runPerformanceBenchmarks(const std::unordered_map<std::string, Pokemon> &cardMap,
                              const std::vector<Pokemon> &deckTemplate,
                              int decisionTreeDepth,
                              int monteCarloSamples,
                              int repetitions) {
    GameState benchmarkState = buildBenchmarkState(cardMap, deckTemplate);
    const int availableThreads = std::max(1, omp_get_max_threads());
    const std::vector<int> threadCounts = buildThreadCounts(availableThreads);

    omp_set_dynamic(0);

    std::cout << "\n=== Performance Benchmark Mode ===" << std::endl;
    std::cout << "Representative benchmark state built from project card data." << std::endl;
    std::cout << "Top-level decision branches: "
              << generateNextStates(benchmarkState).size() << std::endl;
    std::cout << "Decision-tree depth: " << decisionTreeDepth
              << ", Monte Carlo samples: " << monteCarloSamples
              << ", repetitions per measurement: " << repetitions << std::endl;
    std::cout << "Available OpenMP threads: " << availableThreads << std::endl;

    double serialDecisionScore = 0.0;
    const double serialDecisionTime = measureAverageSeconds(
        [&benchmarkState, decisionTreeDepth]() {
            return simulateDecisionTreeSequential(benchmarkState, decisionTreeDepth);
        },
        repetitions,
        serialDecisionScore
    );

    printBenchmarkHeader("Decision Tree Benchmark");
    printBenchmarkRow(1, serialDecisionTime, 1.0, 1.0, serialDecisionScore);

    for (int threads : threadCounts) {
        if (threads == 1) {
            continue;
        }
        double parallelDecisionScore = 0.0;
        omp_set_num_threads(threads);
        const double parallelDecisionTime = measureAverageSeconds(
            [&benchmarkState, decisionTreeDepth]() {
                return simulateDecisionTree(benchmarkState, decisionTreeDepth);
            },
            repetitions,
            parallelDecisionScore
        );
        printBenchmarkRow(
            threads,
            parallelDecisionTime,
            serialDecisionTime / parallelDecisionTime,
            (serialDecisionTime / parallelDecisionTime) / static_cast<double>(threads),
            parallelDecisionScore
        );
    }

    double serialMonteCarloScore = 0.0;
    const double serialMonteCarloTime = measureAverageSeconds(
        [&benchmarkState, monteCarloSamples]() {
            return monteCarloSimulationSequential(benchmarkState, monteCarloSamples);
        },
        repetitions,
        serialMonteCarloScore
    );

    printBenchmarkHeader("Monte Carlo Benchmark");
    printBenchmarkRow(1, serialMonteCarloTime, 1.0, 1.0, serialMonteCarloScore);

    for (int threads : threadCounts) {
        if (threads == 1) {
            continue;
        }
        double parallelMonteCarloScore = 0.0;
        omp_set_num_threads(threads);
        const double parallelMonteCarloTime = measureAverageSeconds(
            [&benchmarkState, monteCarloSamples]() {
                return monteCarloSimulation(benchmarkState, monteCarloSamples);
            },
            repetitions,
            parallelMonteCarloScore
        );
        printBenchmarkRow(
            threads,
            parallelMonteCarloTime,
            serialMonteCarloTime / parallelMonteCarloTime,
            (serialMonteCarloTime / parallelMonteCarloTime) / static_cast<double>(threads),
            parallelMonteCarloScore
        );
    }
}
