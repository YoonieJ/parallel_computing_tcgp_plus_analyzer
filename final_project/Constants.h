#ifndef CONSTANTS_H
#define CONSTANTS_H

// Core deck and gameplay limits.
const int DECK_SIZE = 20;          // Pokémon TCG Pocket decks contain 20 cards.
const int MAX_CARD_COPIES = 2;     // No more than two copies of the same card.
const int MAX_BENCH = 3;           // Maximum of 3 Pokémon on the bench.
const int MAX_HAND_SIZE = DECK_SIZE;
const int INITIAL_HAND_SIZE = 5;   // Starting hand size.
const int MAX_FLIP = 10;           // Maximum number of coin flips

#endif
