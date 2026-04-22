#include "GameSimulation.h"

#include "Constants.h"
#include "FileParser.h"
#include "ParallelUtils.h"
#include "Utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

std::vector<std::string> allMetaDecks;

namespace {

struct SetupOption {
    std::string prefix;
    GameState state;
    int retreatDiscount = 0;
};

struct CandidateAction {
    std::string label;
    GameState state;
};

double clampProbability(double value) {
    return std::max(0.0, std::min(1.0, value));
}

std::string composeLabel(const std::string &prefix, const std::string &action) {
    return prefix.empty() ? action : prefix + action;
}

int countAttachedEnergy(const Pokemon &pokemon) {
    int totalEnergy = 0;
    for (const auto &energy : pokemon.attachedEnergy) {
        totalEnergy += energy.amount;
    }
    return totalEnergy;
}

int countEnergyOfType(const Pokemon &pokemon, const std::string &energyType) {
    int totalEnergy = 0;
    const std::string normalizedType = normalize(energyType);
    for (const auto &energy : pokemon.attachedEnergy) {
        if (normalize(energy.energyType) == normalizedType) {
            totalEnergy += energy.amount;
        }
    }
    return totalEnergy;
}

void healPokemon(Pokemon &pokemon, int amount) {
    if (pokemon.name.empty() || amount <= 0) {
        return;
    }
    pokemon.hp = std::min(pokemon.maxHp, pokemon.hp + amount);
}

void damagePokemon(Pokemon &pokemon, int amount) {
    if (pokemon.name.empty() || amount <= 0) {
        return;
    }
    pokemon.hp = std::max(0, pokemon.hp - amount);
}

void clearAttachments(Pokemon &activePokemon, std::vector<Pokemon> &bench) {
    activePokemon.attachedEnergy.clear();
    for (auto &pokemon : bench) {
        pokemon.attachedEnergy.clear();
    }
}

Pokemon *findPokemonOnSide(Pokemon &activePokemon,
                           std::vector<Pokemon> &bench,
                           const std::string &pokemonName) {
    if (!activePokemon.name.empty() &&
        normalize(activePokemon.name) == normalize(pokemonName)) {
        return &activePokemon;
    }

    for (auto &pokemon : bench) {
        if (!pokemon.name.empty() && normalize(pokemon.name) == normalize(pokemonName)) {
            return &pokemon;
        }
    }

    return nullptr;
}

void rebuildAttachmentSummaryForSide(const Pokemon &activePokemon,
                                     const std::vector<Pokemon> &bench,
                                     std::vector<EnergyAttachment> &summary) {
    summary.clear();

    auto appendPokemon = [&summary](const Pokemon &pokemon) {
        for (const auto &energy : pokemon.attachedEnergy) {
            summary.push_back({pokemon.name, energy.energyType, energy.amount});
        }
    };

    appendPokemon(activePokemon);
    for (const auto &pokemon : bench) {
        appendPokemon(pokemon);
    }
}

void rebuildAttachmentSummaries(GameState &state) {
    rebuildAttachmentSummaryForSide(state.activePokemon, state.bench, state.yourAttachments);
    rebuildAttachmentSummaryForSide(state.opponentActivePokemon, state.opponentBench, state.oppAttachments);
}

void applyAttachmentLine(Pokemon &activePokemon,
                         std::vector<Pokemon> &bench,
                         const std::string &token) {
    if (trim(token).empty()) {
        return;
    }

    const auto parts = splitAndTrim(token, ':');
    if (parts.size() != 3) {
        std::cerr << "Invalid energy attachment format: " << token << std::endl;
        return;
    }

    Pokemon *pokemon = findPokemonOnSide(activePokemon, bench, parts[0]);
    if (pokemon == nullptr) {
        std::cerr << "Error: Pokémon '" << parts[0] << "' not found on the selected board." << std::endl;
        return;
    }

    pokemon->attachedEnergy.emplace_back(parts[1], std::max(0, parseIntOrZero(parts[2])));
}

void applyAttachmentInputToSide(Pokemon &activePokemon,
                                std::vector<Pokemon> &bench,
                                const std::string &input) {
    clearAttachments(activePokemon, bench);
    for (const auto &token : splitAndTrim(input, ',')) {
        applyAttachmentLine(activePokemon, bench, token);
    }
}

bool hasRequiredEnergy(const Pokemon &pokemon, const Skill &skill) {
    if (pokemon.name.empty()) {
        return false;
    }

    const int totalEnergy = countAttachedEnergy(pokemon);
    int requiredTotal = 0;

    for (const auto &requirement : skill.energyRequirements) {
        requiredTotal += requirement.amount;
        if (normalize(requirement.energyType) != normalize("Colorless") &&
            countEnergyOfType(pokemon, requirement.energyType) < requirement.amount) {
            return false;
        }
    }

    return totalEnergy >= requiredTotal;
}

int legalAttackCount(const Pokemon &pokemon) {
    int count = 0;
    for (const auto &skill : pokemon.skills) {
        if (hasRequiredEnergy(pokemon, skill)) {
            ++count;
        }
    }
    return count;
}

int expectedSkillDamage(const Pokemon &attacker,
                        const Pokemon &defender,
                        const Skill &skill) {
    int damage = skill.dmg;
    damage += skill.specialEffect.extraDmg;
    damage += skill.specialEffect.damagePerEnergyAttached * countAttachedEnergy(attacker);

    if (defender.isPoisoned) {
        damage += skill.specialEffect.extraDmgIfPoisoned;
    }
    if (defender.isParalyzed) {
        damage += skill.specialEffect.extraDmgIfParalyzed;
    }
    if (skill.specialEffect.doCoinFlips && skill.specialEffect.damagePerFlip > 0) {
        const int flips = std::max(1, skill.specialEffect.numFlips > 0 ? skill.specialEffect.numFlips : skill.maxFlip);
        damage += static_cast<int>(std::round(0.5 * static_cast<double>(flips) *
                                              static_cast<double>(skill.specialEffect.damagePerFlip)));
    }
    if (!defender.weakness.empty() &&
        normalize(defender.weakness) == normalize(attacker.type)) {
        damage += 20;
    }

    return std::max(0, damage);
}

double scorePokemon(const Pokemon &pokemon, bool isActive) {
    if (pokemon.name.empty()) {
        return 0.0;
    }

    const double healthRatio =
        pokemon.maxHp > 0 ? static_cast<double>(pokemon.hp) / static_cast<double>(pokemon.maxHp) : 0.0;

    int bestDamage = 0;
    for (const auto &skill : pokemon.skills) {
        if (hasRequiredEnergy(pokemon, skill)) {
            bestDamage = std::max(bestDamage, skill.dmg + skill.specialEffect.extraDmg);
        }
    }

    double score = 55.0 * healthRatio;
    score += 0.20 * static_cast<double>(pokemon.maxHp);
    score += 11.0 * static_cast<double>(countAttachedEnergy(pokemon));
    score += 14.0 * static_cast<double>(legalAttackCount(pokemon));
    score += 0.18 * static_cast<double>(bestDamage);
    score += pokemon.isEx ? 8.0 : 0.0;
    score += isActive ? 8.0 : 4.0;

    if (pokemon.isPoisoned) {
        score -= 10.0;
    }
    if (pokemon.isParalyzed) {
        score -= 14.0;
    }

    return score;
}

std::size_t chooseBestBenchIndex(const std::vector<Pokemon> &bench) {
    std::size_t bestIndex = 0;
    double bestScore = -std::numeric_limits<double>::infinity();

    for (std::size_t i = 0; i < bench.size(); ++i) {
        const double score = scorePokemon(bench[i], true);
        if (score > bestScore) {
            bestScore = score;
            bestIndex = i;
        }
    }

    return bestIndex;
}

void promoteBestBench(Pokemon &activePokemon,
                      std::vector<Pokemon> &bench,
                      bool &sideLost) {
    if (activePokemon.name.empty() || activePokemon.hp > 0) {
        return;
    }

    if (bench.empty()) {
        sideLost = true;
        activePokemon = Pokemon();
        return;
    }

    const std::size_t nextActiveIndex = chooseBestBenchIndex(bench);
    activePokemon = bench[nextActiveIndex];
    bench.erase(bench.begin() + static_cast<std::ptrdiff_t>(nextActiveIndex));
}

void resolveKnockouts(GameState &state) {
    if (!state.playerWon && !state.opponentWon) {
        promoteBestBench(state.opponentActivePokemon, state.opponentBench, state.playerWon);
        promoteBestBench(state.activePokemon, state.bench, state.opponentWon);
    }
    rebuildAttachmentSummaries(state);
}

void applyBetweenTurnStatus(GameState &state) {
    if (state.playerWon || state.opponentWon) {
        return;
    }

    if (state.activePokemon.isPoisoned) {
        damagePokemon(state.activePokemon, 10);
    }
    if (state.opponentActivePokemon.isPoisoned) {
        damagePokemon(state.opponentActivePokemon, 10);
    }
    resolveKnockouts(state);
}

void finishTurn(GameState &state) {
    applyBetweenTurnStatus(state);
    if (state.playerWon || state.opponentWon) {
        return;
    }

    state.playerTurn = !state.playerTurn;
    state.supporterUsedThisTurn = false;
    rebuildAttachmentSummaries(state);
}

void consumeEnergy(Pokemon &pokemon, int amount) {
    for (auto &energy : pokemon.attachedEnergy) {
        if (amount <= 0) {
            break;
        }
        const int removed = std::min(amount, energy.amount);
        energy.amount -= removed;
        amount -= removed;
    }

    pokemon.attachedEnergy.erase(
        std::remove_if(pokemon.attachedEnergy.begin(),
                       pokemon.attachedEnergy.end(),
                       [](const EnergyRequirement &energy) { return energy.amount <= 0; }),
        pokemon.attachedEnergy.end()
    );
}

bool canRetreat(const Pokemon &pokemon, int retreatDiscount = 0) {
    const int retreatCost = std::max(0, pokemon.retreatCost - retreatDiscount);
    return countAttachedEnergy(pokemon) >= retreatCost;
}

void applyRetreatCost(Pokemon &pokemon, int retreatDiscount = 0) {
    consumeEnergy(pokemon, std::max(0, pokemon.retreatCost - retreatDiscount));
}

Pokemon evolvePokemon(const Pokemon &currentPokemon, const Pokemon &evolutionCard) {
    Pokemon evolvedPokemon = evolutionCard;
    const int damageTaken = std::max(0, currentPokemon.maxHp - currentPokemon.hp);
    evolvedPokemon.hp = std::max(0, evolvedPokemon.maxHp - damageTaken);
    evolvedPokemon.attachedEnergy = currentPokemon.attachedEnergy;
    evolvedPokemon.isParalyzed = false;
    evolvedPokemon.isPoisoned = false;
    return evolvedPokemon;
}

bool isEvolutionFor(const Pokemon &basePokemon, const Pokemon &candidateEvolution) {
    return !candidateEvolution.prevEvo.empty() &&
           normalize(candidateEvolution.prevEvo) == normalize(basePokemon.name);
}

void drawCardsFromDeck(GameState &state, int count) {
    const int cardsToDraw = std::min(count, static_cast<int>(state.deck.size()));
    for (int i = 0; i < cardsToDraw && static_cast<int>(state.hand.size()) < MAX_HAND_SIZE; ++i) {
        state.hand.push_back(state.deck.front());
        state.deck.erase(state.deck.begin());
    }
}

Pokemon findFirstBasicPokemon(const std::vector<Pokemon> &deck) {
    auto it = std::find_if(deck.begin(), deck.end(), [](const Pokemon &card) {
        return card.cardType == 0 && card.stage == 0;
    });
    return it != deck.end() ? *it : Pokemon();
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

void addAttachment(Pokemon &pokemon,
                   const std::string &energyType,
                   int amount) {
    pokemon.attachedEnergy.emplace_back(energyType, amount);
}

GameState buildBenchmarkState(const std::unordered_map<std::string, Pokemon> &cardMap,
                              const std::vector<Pokemon> &deckTemplate) {
    GameState state;
    state.deck = deckTemplate;
    state.turn = 4;
    state.firstTurn = false;
    state.playerTurn = true;

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

    addAttachment(state.activePokemon, state.activePokemon.type.empty() ? "Grass" : state.activePokemon.type, 2);
    addAttachment(state.activePokemon, "Colorless", 2);

    if (!state.bench.empty()) {
        addAttachment(state.bench[0], state.bench[0].type.empty() ? "Grass" : state.bench[0].type, 2);
    }
    if (state.bench.size() > 1) {
        addAttachment(state.bench[1], state.bench[1].type.empty() ? "Grass" : state.bench[1].type, 1);
    }
    if (state.bench.size() > 2) {
        addAttachment(state.bench[2], "Colorless", 1);
    }

    addAttachment(state.opponentActivePokemon,
                  state.opponentActivePokemon.type.empty() ? "Psychic" : state.opponentActivePokemon.type,
                  3);
    for (std::size_t i = 0; i < state.opponentBench.size(); ++i) {
        if (!state.opponentBench[i].name.empty()) {
            addAttachment(state.opponentBench[i],
                          state.opponentBench[i].type.empty() ? "Colorless" : state.opponentBench[i].type,
                          i == 0 ? 2 : 1);
        }
    }

    rebuildAttachmentSummaries(state);
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

void applySkillToState(GameState &state, const Skill &skill, bool playerAttacking) {
    Pokemon &attacker = playerAttacking ? state.activePokemon : state.opponentActivePokemon;
    Pokemon &defender = playerAttacking ? state.opponentActivePokemon : state.activePokemon;
    std::vector<Pokemon> &defenderBench = playerAttacking ? state.opponentBench : state.bench;

    damagePokemon(defender, expectedSkillDamage(attacker, defender, skill));

    if (!skill.specialEffect.doCoinFlips && skill.specialEffect.paralyzeOpp) {
        defender.isParalyzed = true;
    }
    if (skill.specialEffect.poisonOpp) {
        defender.isPoisoned = true;
    }
    if (skill.specialEffect.heal > 0) {
        healPokemon(attacker, skill.specialEffect.heal);
    }
    if (skill.specialEffect.benchedDamage > 0 && !defenderBench.empty()) {
        const int targets = skill.specialEffect.numBenched > 0
            ? std::min(skill.specialEffect.numBenched, static_cast<int>(defenderBench.size()))
            : static_cast<int>(defenderBench.size());
        for (int i = 0; i < targets; ++i) {
            damagePokemon(defenderBench[static_cast<std::size_t>(i)], skill.specialEffect.benchedDamage);
        }
    }
    if (skill.energyDrop > 0) {
        consumeEnergy(attacker, skill.energyDrop);
    }
    if (skill.specialEffect.switchOutOpp && !defenderBench.empty()) {
        const std::size_t switchIndex = chooseBestBenchIndex(defenderBench);
        std::swap(defender, defenderBench[switchIndex]);
    }

    resolveKnockouts(state);
}

std::vector<SetupOption> buildPlayerSetupOptions(const GameState &state) {
    std::vector<SetupOption> options;
    options.push_back({"", state, 0});

    for (std::size_t handIndex = 0; handIndex < state.hand.size(); ++handIndex) {
        const Pokemon &card = state.hand[handIndex];

        if (card.cardType == 0 && card.stage == 0 && state.bench.size() < MAX_BENCH) {
            SetupOption option{"Bench " + card.name + ", then ", state, 0};
            option.state.bench.push_back(card);
            option.state.hand.erase(option.state.hand.begin() + static_cast<std::ptrdiff_t>(handIndex));
            rebuildAttachmentSummaries(option.state);
            options.push_back(std::move(option));
        }

        if (card.cardType == 0 && isEvolutionFor(state.activePokemon, card)) {
            SetupOption option{"Evolve active into " + card.name + ", then ", state, 0};
            option.state.activePokemon = evolvePokemon(state.activePokemon, card);
            option.state.hand.erase(option.state.hand.begin() + static_cast<std::ptrdiff_t>(handIndex));
            rebuildAttachmentSummaries(option.state);
            options.push_back(std::move(option));
        }

        for (std::size_t benchIndex = 0; benchIndex < state.bench.size(); ++benchIndex) {
            if (card.cardType == 0 && isEvolutionFor(state.bench[benchIndex], card)) {
                SetupOption option{"Evolve " + state.bench[benchIndex].name + " into " + card.name + ", then ",
                                   state, 0};
                option.state.bench[benchIndex] = evolvePokemon(state.bench[benchIndex], card);
                option.state.hand.erase(option.state.hand.begin() + static_cast<std::ptrdiff_t>(handIndex));
                rebuildAttachmentSummaries(option.state);
                options.push_back(std::move(option));
            }
        }

        if (card.cardType == 2) {
            const std::string normalizedName = normalize(card.name);

            if (normalizedName == normalize("Poke Ball")) {
                Pokemon basicPokemon = findFirstBasicPokemon(state.deck);
                if (!basicPokemon.name.empty() && state.hand.size() < MAX_HAND_SIZE) {
                    SetupOption option{"Use Poke Ball for " + basicPokemon.name + ", then ", state, 0};
                    removeFirstCardByName(option.state.hand, card.name);
                    option.state.hand.push_back(basicPokemon);
                    removeFirstCardByName(option.state.deck, basicPokemon.name);
                    options.push_back(std::move(option));
                }
            } else if (normalizedName == normalize("Potion")) {
                if (!state.activePokemon.name.empty() && state.activePokemon.hp < state.activePokemon.maxHp) {
                    SetupOption option{"Use Potion on " + state.activePokemon.name + ", then ", state, 0};
                    removeFirstCardByName(option.state.hand, card.name);
                    healPokemon(option.state.activePokemon, 20);
                    options.push_back(std::move(option));
                }
            } else if (normalizedName == normalize("X Speed")) {
                SetupOption option{"Use X Speed, then ", state, 1};
                removeFirstCardByName(option.state.hand, card.name);
                options.push_back(std::move(option));
            } else {
                SetupOption option{"Use " + card.name + ", then ", state, 0};
                removeFirstCardByName(option.state.hand, card.name);
                options.push_back(std::move(option));
            }
        }

        if (card.cardType == 1 && !state.supporterUsedThisTurn) {
            const std::string normalizedName = normalize(card.name);

            if (normalizedName == normalize("Professor's Research")) {
                SetupOption option{"Use Professor's Research, then ", state, 0};
                removeFirstCardByName(option.state.hand, card.name);
                drawCardsFromDeck(option.state, 2);
                option.state.supporterUsedThisTurn = true;
                options.push_back(std::move(option));
            } else if (normalizedName == normalize("Sabrina") && !state.opponentBench.empty()) {
                for (std::size_t benchIndex = 0; benchIndex < state.opponentBench.size(); ++benchIndex) {
                    SetupOption option{"Use Sabrina to force " + state.opponentBench[benchIndex].name + " active, then ",
                                       state, 0};
                    removeFirstCardByName(option.state.hand, card.name);
                    std::swap(option.state.opponentActivePokemon, option.state.opponentBench[benchIndex]);
                    option.state.supporterUsedThisTurn = true;
                    options.push_back(std::move(option));
                }
            } else {
                SetupOption option{"Use " + card.name + ", then ", state, 0};
                removeFirstCardByName(option.state.hand, card.name);
                option.state.supporterUsedThisTurn = true;
                options.push_back(std::move(option));
            }
        }
    }

    return options;
}

void appendAttackActions(const SetupOption &setup,
                         bool playerActing,
                         std::vector<CandidateAction> &actions) {
    const Pokemon &attacker = playerActing ? setup.state.activePokemon : setup.state.opponentActivePokemon;

    for (const auto &skill : attacker.skills) {
        if (!hasRequiredEnergy(attacker, skill)) {
            continue;
        }

        CandidateAction action;
        action.label = composeLabel(
            setup.prefix,
            playerActing ? "Attack with " + skill.skillName : "Opponent attacks with " + skill.skillName
        );
        action.state = setup.state;
        applySkillToState(action.state, skill, playerActing);
        finishTurn(action.state);
        actions.push_back(std::move(action));
    }
}

void appendRetreatActions(const SetupOption &setup,
                          bool playerActing,
                          std::vector<CandidateAction> &actions) {
    const Pokemon &currentActive = playerActing ? setup.state.activePokemon : setup.state.opponentActivePokemon;
    const std::vector<Pokemon> &currentBench = playerActing ? setup.state.bench : setup.state.opponentBench;

    if (currentBench.empty() || !canRetreat(currentActive, setup.retreatDiscount)) {
        return;
    }

    for (std::size_t benchIndex = 0; benchIndex < currentBench.size(); ++benchIndex) {
        GameState switchedState = setup.state;
        Pokemon &activePokemon = playerActing ? switchedState.activePokemon : switchedState.opponentActivePokemon;
        std::vector<Pokemon> &bench = playerActing ? switchedState.bench : switchedState.opponentBench;

        applyRetreatCost(activePokemon, setup.retreatDiscount);
        std::swap(activePokemon, bench[benchIndex]);
        rebuildAttachmentSummaries(switchedState);

        bool addedAttackAfterRetreat = false;
        for (const auto &skill : activePokemon.skills) {
            if (!hasRequiredEnergy(activePokemon, skill)) {
                continue;
            }

            CandidateAction action;
            action.label = composeLabel(
                setup.prefix,
                std::string(playerActing ? "Retreat to " : "Opponent retreats to ") +
                    activePokemon.name + ", then attack with " + skill.skillName
            );
            action.state = switchedState;
            applySkillToState(action.state, skill, playerActing);
            finishTurn(action.state);
            actions.push_back(std::move(action));
            addedAttackAfterRetreat = true;
        }

        if (!addedAttackAfterRetreat) {
            CandidateAction action;
            action.label = composeLabel(
                setup.prefix,
                std::string(playerActing ? "Retreat to " : "Opponent retreats to ") + activePokemon.name
            );
            action.state = switchedState;
            finishTurn(action.state);
            actions.push_back(std::move(action));
        }
    }
}

std::vector<CandidateAction> buildCandidateActions(const GameState &state) {
    if (state.playerWon || state.opponentWon) {
        return {};
    }

    std::vector<CandidateAction> actions;

    if (state.playerTurn) {
        const std::vector<SetupOption> setupOptions = buildPlayerSetupOptions(state);
        for (const auto &setup : setupOptions) {
            appendAttackActions(setup, true, actions);
            appendRetreatActions(setup, true, actions);
        }
    } else {
        const SetupOption base{"", state, 0};
        appendAttackActions(base, false, actions);
        appendRetreatActions(base, false, actions);
    }

    if (actions.empty()) {
        CandidateAction passAction;
        passAction.label = state.playerTurn ? "Pass turn" : "Opponent passes turn";
        passAction.state = state;
        finishTurn(passAction.state);
        actions.push_back(std::move(passAction));
    }

    return actions;
}

bool anyLegalAttackKnocksOut(const Pokemon &attacker, const Pokemon &defender) {
    for (const auto &skill : attacker.skills) {
        if (hasRequiredEnergy(attacker, skill) &&
            expectedSkillDamage(attacker, defender, skill) >= defender.hp) {
            return true;
        }
    }
    return false;
}

size_t pickDeckIndex(size_t deckSize, int simulationIndex) {
    std::mt19937 generator(0xC0FFEEu + static_cast<unsigned int>(simulationIndex));
    std::uniform_int_distribution<size_t> pickCard(0, deckSize - 1);
    return pickCard(generator);
}

int promptMenuChoice(const std::string &prompt, int minimum, int maximum, bool allowZero = false) {
    std::string input;
    while (true) {
        std::cout << prompt;
        std::getline(std::cin, input);
        if (toLower(input) == "exit") {
            std::cout << "Exiting the program. Goodbye!" << std::endl;
            std::exit(0);
        }
        try {
            const int choice = std::stoi(input);
            if (allowZero && choice == 0) {
                return 0;
            }
            if (choice >= minimum && choice <= maximum) {
                return choice;
            }
        } catch (...) {
        }
        std::cerr << "Error: Invalid input." << std::endl;
    }
}

bool promptYesNo(const std::string &prompt) {
    std::string input;
    while (true) {
        std::cout << prompt;
        std::getline(std::cin, input);
        input = toLower(input);
        if (input == "exit") {
            std::cout << "Exiting the program. Goodbye!" << std::endl;
            std::exit(0);
        }
        if (input == "yes" || input == "y") {
            return true;
        }
        if (input == "no" || input == "n") {
            return false;
        }
        std::cerr << "Error: Please answer yes or no." << std::endl;
    }
}

void promptEnergyAttachment(GameState &state) {
    if (!promptYesNo("Do you want to record an energy attachment for this turn? (yes/no): ")) {
        return;
    }

    std::string pokemonName;
    std::string energyType;
    std::string amountInput;

    std::cout << "Attach to which Pokémon? ";
    std::getline(std::cin, pokemonName);
    std::cout << "Energy type: ";
    std::getline(std::cin, energyType);
    std::cout << "Amount (usually 1): ";
    std::getline(std::cin, amountInput);

    Pokemon *target = findPokemonOnSide(state.activePokemon, state.bench, pokemonName);
    if (target == nullptr) {
        std::cerr << "Error: Pokémon '" << pokemonName << "' was not found on your board." << std::endl;
        return;
    }

    target->attachedEnergy.emplace_back(energyType, std::max(1, parseIntOrZero(amountInput)));
    rebuildAttachmentSummaries(state);
}

void placeBenchPokemonFromHand(GameState &state) {
    if (!promptYesNo("Do you want to place a Pokémon from your hand onto the bench? (yes/no): ")) {
        return;
    }

    if (state.bench.size() >= MAX_BENCH) {
        std::cout << "Your bench is full." << std::endl;
        return;
    }

    std::vector<std::size_t> basicIndices;
    for (std::size_t i = 0; i < state.hand.size(); ++i) {
        if (state.hand[i].cardType == 0 && state.hand[i].stage == 0) {
            basicIndices.push_back(i);
            std::cout << "  " << basicIndices.size() << ". " << state.hand[i].name << std::endl;
        }
    }

    if (basicIndices.empty()) {
        std::cout << "No basic Pokémon are available in your hand." << std::endl;
        return;
    }

    const int choice = promptMenuChoice(
        "Select the Pokémon to bench (or press 0 to skip): ",
        1,
        static_cast<int>(basicIndices.size()),
        true
    );
    if (choice == 0) {
        return;
    }

    const std::size_t handIndex = basicIndices[static_cast<std::size_t>(choice - 1)];
    state.bench.push_back(state.hand[handIndex]);
    state.hand.erase(state.hand.begin() + static_cast<std::ptrdiff_t>(handIndex));
}

void evolvePokemonFromHand(GameState &state) {
    if (!promptYesNo("Do you want to evolve one of your Pokémon? (yes/no): ")) {
        return;
    }

    struct EvolutionChoice {
        bool activeTarget = false;
        std::size_t targetIndex = 0;
        std::size_t handIndex = 0;
        std::string description;
    };

    std::vector<EvolutionChoice> choices;

    for (std::size_t handIndex = 0; handIndex < state.hand.size(); ++handIndex) {
        const Pokemon &card = state.hand[handIndex];
        if (card.cardType != 0 || card.prevEvo.empty()) {
            continue;
        }

        if (isEvolutionFor(state.activePokemon, card)) {
            choices.push_back({true, 0, handIndex, state.activePokemon.name + " -> " + card.name});
        }

        for (std::size_t benchIndex = 0; benchIndex < state.bench.size(); ++benchIndex) {
            if (isEvolutionFor(state.bench[benchIndex], card)) {
                choices.push_back({false, benchIndex, handIndex,
                                   state.bench[benchIndex].name + " -> " + card.name});
            }
        }
    }

    if (choices.empty()) {
        std::cout << "No valid evolution plays are available in your hand." << std::endl;
        return;
    }

    std::cout << "Available evolutions:" << std::endl;
    for (std::size_t i = 0; i < choices.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << choices[i].description << std::endl;
    }

    const int choice = promptMenuChoice(
        "Select the evolution to apply (or press 0 to skip): ",
        1,
        static_cast<int>(choices.size()),
        true
    );
    if (choice == 0) {
        return;
    }

    const EvolutionChoice &selected = choices[static_cast<std::size_t>(choice - 1)];
    const Pokemon evolutionCard = state.hand[selected.handIndex];

    if (selected.activeTarget) {
        state.activePokemon = evolvePokemon(state.activePokemon, evolutionCard);
    } else {
        state.bench[selected.targetIndex] = evolvePokemon(state.bench[selected.targetIndex], evolutionCard);
    }

    state.hand.erase(state.hand.begin() + static_cast<std::ptrdiff_t>(selected.handIndex));
    rebuildAttachmentSummaries(state);
}

void useTrainerFromHand(GameState &state, bool supporterCard) {
    std::vector<std::size_t> trainerIndices;
    for (std::size_t i = 0; i < state.hand.size(); ++i) {
        if (state.hand[i].cardType == (supporterCard ? 1 : 2)) {
            trainerIndices.push_back(i);
            std::cout << "  " << trainerIndices.size() << ". " << state.hand[i].name << std::endl;
        }
    }

    if (trainerIndices.empty()) {
        std::cout << "No " << (supporterCard ? "Supporter" : "Item") << " cards are available in your hand." << std::endl;
        return;
    }

    const int choice = promptMenuChoice(
        std::string("Choose a ") + (supporterCard ? "Supporter" : "Item") +
            " to use (or press 0 to cancel): ",
        1,
        static_cast<int>(trainerIndices.size()),
        true
    );
    if (choice == 0) {
        return;
    }

    const std::size_t handIndex = trainerIndices[static_cast<std::size_t>(choice - 1)];
    const Pokemon card = state.hand[handIndex];
    const std::string normalizedName = normalize(card.name);

    if (supporterCard && state.supporterUsedThisTurn) {
        std::cout << "You have already used a Supporter this turn." << std::endl;
        return;
    }

    if (!supporterCard && normalizedName == normalize("Poke Ball")) {
        const Pokemon basicPokemon = findFirstBasicPokemon(state.deck);
        if (basicPokemon.name.empty()) {
            std::cout << "No basic Pokémon remain in your deck for Poke Ball." << std::endl;
        } else if (state.hand.size() >= MAX_HAND_SIZE) {
            std::cout << "Your hand is full." << std::endl;
        } else {
            state.hand.push_back(basicPokemon);
            removeFirstCardByName(state.deck, basicPokemon.name);
            std::cout << "Added " << basicPokemon.name << " to your hand." << std::endl;
        }
    } else if (!supporterCard && normalizedName == normalize("Potion")) {
        healPokemon(state.activePokemon, 20);
        std::cout << state.activePokemon.name << " healed 20 HP." << std::endl;
    } else if (!supporterCard && normalizedName == normalize("X Speed")) {
        std::cout << "X Speed is applied. Your next retreat this turn is treated as one energy cheaper in the simulator." << std::endl;
    } else if (supporterCard && normalizedName == normalize("Professor's Research")) {
        drawCardsFromDeck(state, 2);
        std::cout << "Drew up to 2 cards with Professor's Research." << std::endl;
    } else if (supporterCard && normalizedName == normalize("Sabrina")) {
        if (state.opponentBench.empty()) {
            std::cout << "Opponent has no benched Pokémon for Sabrina." << std::endl;
        } else {
            std::cout << "Opponent bench:" << std::endl;
            for (std::size_t i = 0; i < state.opponentBench.size(); ++i) {
                std::cout << "  " << (i + 1) << ". " << state.opponentBench[i].name << std::endl;
            }
            const int benchChoice = promptMenuChoice(
                "Choose which opposing benched Pokémon becomes active: ",
                1,
                static_cast<int>(state.opponentBench.size())
            );
            std::swap(state.opponentActivePokemon, state.opponentBench[static_cast<std::size_t>(benchChoice - 1)]);
        }
    }

    state.hand.erase(state.hand.begin() + static_cast<std::ptrdiff_t>(handIndex));
    if (supporterCard) {
        state.supporterUsedThisTurn = true;
    }
    rebuildAttachmentSummaries(state);
}

void printDeckSummary(const std::vector<Pokemon> &deck) {
    std::unordered_map<std::string, int> counts;
    for (const auto &card : deck) {
        counts[card.name]++;
    }

    for (const auto &entry : counts) {
        std::cout << "  " << entry.first << " x" << entry.second << std::endl;
    }
}

}  // namespace

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
}

std::vector<std::string> filterMetaDecksByVisibleBoard(
    const std::vector<std::string> &visiblePokemons
) {
    if (allMetaDecks.empty()) {
        return {};
    }

    const auto deckMatches = parallel::map<bool>(allMetaDecks.size(), [&](std::size_t index) {
        const auto &deck = allMetaDecks[index];
        std::istringstream deckStream(deck);
        std::unordered_set<std::string> deckPokemons;
        std::string line;

        while (std::getline(deckStream, line)) {
            line = trim(line);
            if (line.empty()) {
                continue;
            }
            const auto tokens = splitAndTrim(line, ',');
            if (tokens.size() == 2) {
                deckPokemons.insert(normalize(tokens[0]));
            }
        }

        for (const auto &visiblePokemon : visiblePokemons) {
            if (deckPokemons.find(normalize(visiblePokemon)) == deckPokemons.end()) {
                return false;
            }
        }
        return true;
    });

    std::vector<std::string> filteredDecks;
    for (std::size_t i = 0; i < allMetaDecks.size(); ++i) {
        if (deckMatches[i]) {
            filteredDecks.push_back(allMetaDecks[i]);
        }
    }

    return filteredDecks.empty() ? allMetaDecks : filteredDecks;
}

void updateMetaDeckGuesses(GameState &state) {
    std::vector<std::string> visiblePokemons;

    if (!state.opponentActivePokemon.name.empty()) {
        visiblePokemons.push_back(state.opponentActivePokemon.name);
    }
    for (const auto &benchPokemon : state.opponentBench) {
        if (!benchPokemon.name.empty()) {
            visiblePokemons.push_back(benchPokemon.name);
        }
    }

    state.oppMetaDeckGuesses = filterMetaDecksByVisibleBoard(visiblePokemons);
}

double evaluateGameState(const GameState &state, int depth) {
    if (state.playerWon) {
        return 1.0;
    }
    if (state.opponentWon) {
        return 0.0;
    }

    double score = 0.0;

    score += scorePokemon(state.activePokemon, true);
    for (const auto &pokemon : state.bench) {
        score += scorePokemon(pokemon, false);
    }
    score += 3.5 * static_cast<double>(state.hand.size());
    score += 1.5 * static_cast<double>(state.deck.size());

    score -= scorePokemon(state.opponentActivePokemon, true);
    for (const auto &pokemon : state.opponentBench) {
        score -= scorePokemon(pokemon, false);
    }

    if (anyLegalAttackKnocksOut(state.activePokemon, state.opponentActivePokemon)) {
        score += 30.0;
    }
    if (anyLegalAttackKnocksOut(state.opponentActivePokemon, state.activePokemon)) {
        score -= 30.0;
    }

    score += state.playerTurn ? 6.0 : -6.0;
    score += 2.0 * static_cast<double>(depth);

    return clampProbability(0.5 + (score / 300.0));
}

double monteCarloSimulation(const GameState &state, int numSimulations) {
    if (numSimulations <= 0) {
        return evaluateGameState(state);
    }

    const double totalOutcome = parallel::sum(numSimulations, [&](int simulationIndex) {
        GameState sampledState = state;

        if (!sampledState.deck.empty() && sampledState.hand.size() < MAX_HAND_SIZE) {
            const std::size_t cardIndex = pickDeckIndex(sampledState.deck.size(), simulationIndex);
            sampledState.hand.push_back(sampledState.deck[cardIndex]);
            sampledState.deck.erase(sampledState.deck.begin() + static_cast<std::ptrdiff_t>(cardIndex));
        }

        const auto actions = buildCandidateActions(sampledState);
        if (!actions.empty()) {
            const std::size_t actionIndex =
                actions.size() == 1 ? 0 : pickDeckIndex(actions.size(), simulationIndex + 17);
            sampledState = actions[actionIndex].state;
        }

        return evaluateGameState(sampledState);
    });

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
            const std::size_t cardIndex = pickDeckIndex(sampledState.deck.size(), i);
            sampledState.hand.push_back(sampledState.deck[cardIndex]);
            sampledState.deck.erase(sampledState.deck.begin() + static_cast<std::ptrdiff_t>(cardIndex));
        }

        const auto actions = buildCandidateActions(sampledState);
        if (!actions.empty()) {
            const std::size_t actionIndex =
                actions.size() == 1 ? 0 : pickDeckIndex(actions.size(), i + 17);
            sampledState = actions[actionIndex].state;
        }

        totalOutcome += evaluateGameState(sampledState);
    }

    return totalOutcome / static_cast<double>(numSimulations);
}

void processRoundInput(GameState &state) {
    state.playerTurn = true;
    state.supporterUsedThisTurn = false;

    std::cout << "\nEnter round information (type 'exit' to terminate):" << std::endl;
    promptEnergyAttachment(state);
    placeBenchPokemonFromHand(state);
    evolvePokemonFromHand(state);

    const int action = promptMenuChoice(
        "Select action (1 = Attack, 2 = Retreat, 3 = Use Item, 4 = Use Supporter, 5 = Nothing): ",
        1,
        5
    );

    switch (action) {
    case 1: {
        if (state.activePokemon.skills.empty()) {
            std::cout << "No skills are available for your active Pokémon." << std::endl;
            break;
        }

        std::vector<std::size_t> legalSkills;
        for (std::size_t i = 0; i < state.activePokemon.skills.size(); ++i) {
            if (hasRequiredEnergy(state.activePokemon, state.activePokemon.skills[i])) {
                legalSkills.push_back(i);
                std::cout << "  " << legalSkills.size() << ". "
                          << state.activePokemon.skills[i].skillName
                          << " (expected damage: "
                          << expectedSkillDamage(state.activePokemon,
                                                 state.opponentActivePokemon,
                                                 state.activePokemon.skills[i]) << ")"
                          << std::endl;
            }
        }

        if (legalSkills.empty()) {
            std::cout << "Your active Pokémon does not have enough energy to attack." << std::endl;
            break;
        }

        const int skillChoice = promptMenuChoice(
            "Choose the skill to use: ",
            1,
            static_cast<int>(legalSkills.size())
        );
        const Skill &usedSkill = state.activePokemon.skills[legalSkills[static_cast<std::size_t>(skillChoice - 1)]];
        applySkillToState(state, usedSkill, true);
        std::cout << state.activePokemon.name << " used " << usedSkill.skillName << "." << std::endl;
        break;
    }

    case 2: {
        if (state.bench.empty()) {
            std::cout << "No benched Pokémon are available." << std::endl;
            break;
        }
        if (!canRetreat(state.activePokemon)) {
            std::cout << "Your active Pokémon does not have enough energy to retreat." << std::endl;
            break;
        }

        std::cout << "Benched Pokémon:" << std::endl;
        for (std::size_t i = 0; i < state.bench.size(); ++i) {
            std::cout << "  " << (i + 1) << ". " << state.bench[i].name << std::endl;
        }
        const int benchChoice = promptMenuChoice(
            "Choose which benched Pokémon becomes active: ",
            1,
            static_cast<int>(state.bench.size())
        );
        applyRetreatCost(state.activePokemon);
        std::swap(state.activePokemon, state.bench[static_cast<std::size_t>(benchChoice - 1)]);
        rebuildAttachmentSummaries(state);

        if (promptYesNo("Did you attack after retreating? (yes/no): ")) {
            processRoundInput(state);
            return;
        }
        break;
    }

    case 3:
        useTrainerFromHand(state, false);
        if (promptYesNo("Did you attack after using the Item card? (yes/no): ")) {
            processRoundInput(state);
            return;
        }
        break;

    case 4:
        useTrainerFromHand(state, true);
        if (promptYesNo("Did you attack after using the Supporter card? (yes/no): ")) {
            processRoundInput(state);
            return;
        }
        break;

    case 5:
        std::cout << "No action recorded for this turn." << std::endl;
        break;
    }

    resolveKnockouts(state);
    updateMetaDeckGuesses(state);
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
        if (line.empty()) {
            continue;
        }

        const auto tokens = splitAndTrim(line, ',');
        if (tokens.size() < 2) {
            std::cerr << "Warning: Deck entry malformed: '" << line << "'" << std::endl;
            continue;
        }

        const std::string cardName = tokens[0];
        const int requestedCount = parseIntOrZero(tokens[1]);
        if (requestedCount <= 0) {
            std::cerr << "Warning: Invalid count for '" << cardName << "'." << std::endl;
            continue;
        }

        auto it = cardMap.find(normalize(cardName));
        if (it == cardMap.end()) {
            std::cerr << "Warning: Card '" << cardName << "' not found in card database." << std::endl;
            continue;
        }

        const int allowedCount = std::min(requestedCount, MAX_CARD_COPIES);
        for (int i = 0; i < allowedCount && deck.size() < DECK_SIZE; ++i) {
            const std::string normalizedName = normalize(cardName);
            if (copyCounts[normalizedName] >= MAX_CARD_COPIES) {
                break;
            }
            deck.push_back(it->second);
            copyCounts[normalizedName]++;
        }
    }

    if (deck.size() != DECK_SIZE) {
        std::cerr << "Warning: Loaded deck has " << deck.size()
                  << " cards; expected " << DECK_SIZE << "." << std::endl;
    }
}

void validateDeck(const std::vector<Pokemon> &deck) {
    std::unordered_map<std::string, int> cardCounts;

    for (std::size_t i = 0; i < deck.size(); ++i) {
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
    std::cout << "\nEnter your initial hand (" << INITIAL_HAND_SIZE << " cards, separated by commas): ";
    std::string input;
    std::getline(std::cin, input);

    const auto cardNames = splitAndTrim(input, ',');
    if (cardNames.size() != INITIAL_HAND_SIZE) {
        std::cerr << "Error: You must select exactly " << INITIAL_HAND_SIZE << " cards." << std::endl;
        return;
    }

    std::vector<std::size_t> deckIndices;
    for (const auto &cardName : cardNames) {
        auto it = std::find_if(state.deck.begin(), state.deck.end(),
                               [&cardName](const Pokemon &card) {
                                   return normalize(card.name) == normalize(cardName);
                               });
        if (it == state.deck.end()) {
            std::cerr << "Error: Card '" << cardName << "' is not in your deck." << std::endl;
            return;
        }
        deckIndices.push_back(static_cast<std::size_t>(std::distance(state.deck.begin(), it)));
    }

    std::sort(deckIndices.begin(), deckIndices.end(), std::greater<std::size_t>());
    for (std::size_t index : deckIndices) {
        state.hand.push_back(state.deck[index]);
        state.deck.erase(state.deck.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

double simulateDecisionTree(const GameState &state, int depth) {
    if (depth <= 0 || state.playerWon || state.opponentWon) {
        return evaluateGameState(state, depth);
    }

    const auto actions = buildCandidateActions(state);
    if (actions.empty()) {
        return evaluateGameState(state, depth);
    }

    const auto childScores = parallel::map<double>(actions.size(), [&](std::size_t index) {
        return simulateDecisionTreeSequential(actions[index].state, depth - 1);
    });

    if (state.playerTurn) {
        return *std::max_element(childScores.begin(), childScores.end());
    }
    return *std::min_element(childScores.begin(), childScores.end());
}

double simulateDecisionTreeSequential(const GameState &state, int depth) {
    if (depth <= 0 || state.playerWon || state.opponentWon) {
        return evaluateGameState(state, depth);
    }

    const auto actions = buildCandidateActions(state);
    if (actions.empty()) {
        return evaluateGameState(state, depth);
    }

    double bestScore = state.playerTurn ? 0.0 : 1.0;
    for (const auto &action : actions) {
        const double childScore = simulateDecisionTreeSequential(action.state, depth - 1);
        if (state.playerTurn) {
            bestScore = std::max(bestScore, childScore);
        } else {
            bestScore = std::min(bestScore, childScore);
        }
    }
    return bestScore;
}

void preStartConfiguration(const GameState &state) {
    std::cout << "Pre-Start Deck Summary:" << std::endl;
    printDeckSummary(state.deck);
}

void preFirstRoundConfiguration(GameState &state) {
    std::cout << "\nPre-1st Round Configuration:" << std::endl;
    const bool goingFirst = promptYesNo("Did you win the opening coin flip? (yes/no): ");
    state.firstTurn = goingFirst;
    state.playerTurn = true;

    std::cout << "Your Initial Hand:" << std::endl;
    for (const auto &card : state.hand) {
        std::cout << "  " << card.name << std::endl;
    }
}

void postFirstRoundUpdate(GameState &state, const std::unordered_map<std::string, Pokemon> &cardMap) {
    std::cout << "\nPost-1st Round Update:\n";

    std::string input;

    std::cout << "Enter your Active Pokémon: ";
    std::getline(std::cin, input);
    if (!input.empty()) {
        auto it = cardMap.find(normalize(input));
        if (it != cardMap.end()) {
            state.activePokemon = it->second;
        }
    }

    std::cout << "Enter your Benched Pokémon (comma-separated, blank for none): ";
    std::getline(std::cin, input);
    state.bench.clear();
    for (const auto &name : splitAndTrim(input, ',')) {
        if (name.empty()) {
            continue;
        }
        auto it = cardMap.find(normalize(name));
        if (it != cardMap.end()) {
            state.bench.push_back(it->second);
        } else {
            std::cerr << "Error: Pokémon '" << name << "' not found in card database." << std::endl;
        }
    }

    std::cout << "Enter your Energy attachments (Pokemon:EnergyType:Amount, ...): ";
    std::getline(std::cin, input);
    applyAttachmentInputToSide(state.activePokemon, state.bench, input);

    std::cout << "Enter opponent's Active Pokémon: ";
    std::getline(std::cin, input);
    if (!input.empty()) {
        auto it = cardMap.find(normalize(input));
        if (it != cardMap.end()) {
            state.opponentActivePokemon = it->second;
        }
    }

    std::cout << "Enter opponent's Benched Pokémon (comma-separated, blank for none): ";
    std::getline(std::cin, input);
    state.opponentBench.clear();
    for (const auto &name : splitAndTrim(input, ',')) {
        if (name.empty()) {
            continue;
        }
        auto it = cardMap.find(normalize(name));
        if (it != cardMap.end()) {
            state.opponentBench.push_back(it->second);
        } else {
            std::cerr << "Error: Pokémon '" << name << "' not found in card database." << std::endl;
        }
    }

    std::cout << "Enter opponent's Energy attachments (Pokemon:EnergyType:Amount, ...): ";
    std::getline(std::cin, input);
    applyAttachmentInputToSide(state.opponentActivePokemon, state.opponentBench, input);

    std::cout << "How many attacks were used this round? ";
    int attackCount = 0;
    std::cin >> attackCount;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    state.attacksThisRound.clear();
    for (int i = 0; i < attackCount; ++i) {
        AttackRecord record;
        std::cout << "Attacker name: ";
        std::getline(std::cin, record.attacker);
        std::cout << "Move used: ";
        std::getline(std::cin, record.moveName);
        std::cout << "Target: ";
        std::getline(std::cin, record.target);
        std::cout << "Effects (semicolon-separated): ";
        std::string effects;
        std::getline(std::cin, effects);
        record.effects = splitAndTrim(effects, ';');
        state.attacksThisRound.push_back(record);
    }

    rebuildAttachmentSummaries(state);
    updateMetaDeckGuesses(state);
}

void preEveryRoundConfiguration(GameState &state) {
    std::cout << "\nPre-Every Round Configuration:\n";

    std::cout << "Enter the name of the drawn card (or 'none'): ";
    std::string drawnCard;
    std::getline(std::cin, drawnCard);

    if (drawnCard.empty() || normalize(drawnCard) == normalize("none")) {
        return;
    }

    auto it = std::find_if(state.deck.begin(), state.deck.end(),
                           [&drawnCard](const Pokemon &card) {
                               return normalize(card.name) == normalize(drawnCard);
                           });
    if (it == state.deck.end()) {
        std::cerr << "Error: Card '" << drawnCard << "' was not found in your remaining deck." << std::endl;
        return;
    }

    if (state.hand.size() >= MAX_HAND_SIZE) {
        std::cerr << "Error: Hand size limit reached." << std::endl;
        return;
    }

    state.hand.push_back(*it);
    state.deck.erase(it);
}

void postEveryRoundUpdate(GameState &state) {
    state.playerTurn = false;
    std::cout << "\nPost-Every Round Update:\n";

    const int action = promptMenuChoice(
        "Opponent's action (1 = Attack, 2 = Retreat, 3 = Use Item, 4 = Use Supporter, 5 = Nothing): ",
        1,
        5
    );

    switch (action) {
    case 1: {
        if (state.opponentActivePokemon.skills.empty()) {
            std::cout << "Opponent's active Pokémon has no listed skills." << std::endl;
            break;
        }

        std::vector<std::size_t> legalSkills;
        for (std::size_t i = 0; i < state.opponentActivePokemon.skills.size(); ++i) {
            if (hasRequiredEnergy(state.opponentActivePokemon, state.opponentActivePokemon.skills[i])) {
                legalSkills.push_back(i);
                std::cout << "  " << legalSkills.size() << ". "
                          << state.opponentActivePokemon.skills[i].skillName
                          << " (expected damage: "
                          << expectedSkillDamage(state.opponentActivePokemon,
                                                 state.activePokemon,
                                                 state.opponentActivePokemon.skills[i]) << ")"
                          << std::endl;
            }
        }

        if (legalSkills.empty()) {
            std::cout << "Opponent does not currently have enough energy for a listed attack." << std::endl;
            break;
        }

        const int skillChoice = promptMenuChoice(
            "Choose which skill the opponent used: ",
            1,
            static_cast<int>(legalSkills.size())
        );
        const Skill &usedSkill =
            state.opponentActivePokemon.skills[legalSkills[static_cast<std::size_t>(skillChoice - 1)]];
        applySkillToState(state, usedSkill, false);
        std::cout << "Opponent used " << usedSkill.skillName << "." << std::endl;
        break;
    }

    case 2: {
        if (state.opponentBench.empty()) {
            std::cout << "Opponent has no benched Pokémon." << std::endl;
            break;
        }
        if (!canRetreat(state.opponentActivePokemon)) {
            std::cout << "Opponent's active Pokémon cannot retreat with its current energy." << std::endl;
            break;
        }

        std::cout << "Opponent bench:" << std::endl;
        for (std::size_t i = 0; i < state.opponentBench.size(); ++i) {
            std::cout << "  " << (i + 1) << ". " << state.opponentBench[i].name << std::endl;
        }

        const int benchChoice = promptMenuChoice(
            "Choose which opposing benched Pokémon becomes active: ",
            1,
            static_cast<int>(state.opponentBench.size())
        );
        applyRetreatCost(state.opponentActivePokemon);
        std::swap(state.opponentActivePokemon, state.opponentBench[static_cast<std::size_t>(benchChoice - 1)]);
        rebuildAttachmentSummaries(state);
        break;
    }

    case 3:
        std::cout << "Opponent used an Item card." << std::endl;
        break;

    case 4:
        std::cout << "Opponent used a Supporter card." << std::endl;
        break;

    case 5:
        std::cout << "Opponent took no visible action." << std::endl;
        break;
    }

    resolveKnockouts(state);
    updateMetaDeckGuesses(state);
    state.playerTurn = true;
    state.supporterUsedThisTurn = false;
}

void printActionProbabilities(const GameState &state, int simulationDepth) {
    GameState playerState = state;
    playerState.playerTurn = true;
    const auto actions = buildCandidateActions(playerState);

    std::cout << "\n--- Action Probabilities ---\n";
    for (const auto &action : actions) {
        const double probability = simulateDecisionTree(action.state, simulationDepth) * 100.0;
        std::cout << "  " << action.label << ": " << std::fixed << std::setprecision(2)
                  << probability << "% estimated win chance" << std::endl;
    }
}

void runPerformanceBenchmarks(const std::unordered_map<std::string, Pokemon> &cardMap,
                              const std::vector<Pokemon> &deckTemplate,
                              int decisionTreeDepth,
                              int monteCarloSamples,
                              int repetitions) {
    GameState benchmarkState = buildBenchmarkState(cardMap, deckTemplate);
    const int availableThreads = std::max(1, parallel::getMaxThreads());
    const std::vector<int> threadCounts = buildThreadCounts(availableThreads);

    parallel::disableDynamicThreading();

    std::cout << "\n=== Performance Benchmark Mode ===" << std::endl;
    std::cout << "Representative benchmark state built from project card data." << std::endl;
    std::cout << "Top-level decision branches: "
              << buildCandidateActions(benchmarkState).size() << std::endl;
    std::cout << "Decision-tree depth: " << decisionTreeDepth
              << ", Monte Carlo samples: " << monteCarloSamples
              << ", repetitions per measurement: " << repetitions << std::endl;
    std::cout << "Available worker threads: " << availableThreads << std::endl;
#if TCGPPLUS_HAS_OPENMP
    std::cout << "Parallel backend: OpenMP" << std::endl;
#else
    std::cout << "Parallel backend: std::thread fallback" << std::endl;
#endif

    parallel::setThreadCount(1);
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
        parallel::setThreadCount(threads);
        double parallelDecisionScore = 0.0;
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

    parallel::setThreadCount(1);
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
        parallel::setThreadCount(threads);
        double parallelMonteCarloScore = 0.0;
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
