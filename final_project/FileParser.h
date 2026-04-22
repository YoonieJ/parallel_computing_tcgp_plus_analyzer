#ifndef FILEPARSER_H
#define FILEPARSER_H

#include <unordered_map>
#include <string>
#include "PokemonCard.h"
#include "Utils.h"

// Loads card definitions into the normalized card map.
void loadCardMapFromFile(const std::string &filename, std::unordered_map<std::string, Pokemon> &cardMap);
int parseIntOrZero(const std::string &raw);

#endif
