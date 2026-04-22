#include <iostream>
#include <unordered_map>
#include <string>
#include "PokemonCard.h"
#include "FileParser.h"
#include "GameSimulation.h"
#include "Utils.h"

int main(int argc, char *argv[]) {
    std::unordered_map<std::string, Pokemon> cardMap;
    const std::string cardFile = "Cards.txt";
    loadCardMapFromFile(cardFile, cardMap);
    loadAllMetaDecks("metaDecks.txt");
    std::cout << "Total cards loaded from file: " << cardMap.size() << std::endl;

    GameState state;
    const std::string deckFile = "deck.txt";
    loadPresetDeck(deckFile, cardMap, state.deck);
    validateDeck(state.deck);

    if (argc > 1 && std::string(argv[1]) == "--benchmark") {
        runPerformanceBenchmarks(cardMap, state.deck);
        return 0;
    }

    preStartConfiguration(state);

    drawInitialHand(state);
    preFirstRoundConfiguration(state);

    postFirstRoundUpdate(state, cardMap);
    updateMetaDeckGuesses(state);
    if (!state.oppMetaDeckGuesses.empty()) {
        std::cout << "Possible opponent meta-decks:" << std::endl;
        for (const auto &deckGuess : state.oppMetaDeckGuesses) {
            std::cout << deckGuess << std::endl;
        }
    }

    const int maxRounds = 5;
    const int simulationDepth = 3;
    for (int round = 2; round <= maxRounds; ++round) {
        std::cout << "\n===== Round " << round << " =====" << std::endl;
        preEveryRoundConfiguration(state);
        processRoundInput(state);
        postEveryRoundUpdate(state);
        state.playerTurn = true;
        updateMetaDeckGuesses(state);
        printActionProbabilities(state, simulationDepth);
        double winProbability = simulateDecisionTree(state, simulationDepth) * 100;
        std::cout << "Winning probability for this round: " << winProbability << "%" << std::endl;
        state.turn++;
    }

    return 0;
}
