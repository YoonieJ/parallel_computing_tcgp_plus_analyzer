#include "FileParser.h"
#include "Utils.h"
#include <fstream>
#include <iostream>
#include <cctype>
#include <string>

namespace {

void linkEvolutionPointers(std::unordered_map<std::string, Pokemon> &cardMap) {
    for (auto &[_, pokemon] : cardMap) {
        pokemon.preEvolution = nullptr;
        pokemon.postEvolution = nullptr;
    }

    for (auto &[_, pokemon] : cardMap) {
        if (!pokemon.prevEvo.empty()) {
            auto prevIt = cardMap.find(normalize(pokemon.prevEvo));
            if (prevIt != cardMap.end()) {
                pokemon.preEvolution = &prevIt->second;
                prevIt->second.postEvolution = &pokemon;
            }
        }

        if (!pokemon.nextEvo.empty()) {
            auto nextIt = cardMap.find(normalize(pokemon.nextEvo));
            if (nextIt != cardMap.end()) {
                pokemon.postEvolution = &nextIt->second;
                nextIt->second.preEvolution = &pokemon;
            }
        }
    }
}

}

// Parses an integer while tolerating trailing non-digit characters.
int parseIntOrZero(const std::string &raw) {
    std::string s = trim(raw);
    while (!s.empty() && !isdigit(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    if (s.empty()) return 0;
    try {
        return std::stoi(s);
    } catch (...) {
        return 0;
    }
}
void applySkillEffectToken(const std::string &token, Skill &skill) {
    SpecialSkill &effect = skill.specialEffect;
    const std::string normalizedToken = toLower(trim(token));

    if (normalizedToken == "none") return;
    if (normalizedToken == "coinflip:paralyzeopp") {
        effect.doCoinFlips = true;
        effect.numFlips = 1;
        effect.paralyzeOpp = true;
    } else if (normalizedToken.rfind("heal:", 0) == 0) {
        effect.heal = parseIntOrZero(token.substr(5));
    } else if (normalizedToken.rfind("coinflip:", 0) == 0) {
        effect.doCoinFlips = true;
        effect.damagePerFlip = parseIntOrZero(token.substr(9));
    } else if (normalizedToken == "shufflebackifheads") {
        effect.doCoinFlips = true;
        effect.numFlips = 1;
        effect.shuffleOpponentBackIfHeads = true;
    } else if (normalizedToken == "shuffleback") {
        effect.shuffleOpponentBackIfHeads = true;
    } else if (normalizedToken.rfind("damageperenergy:", 0) == 0 ||
               normalizedToken.rfind("energyattached:", 0) == 0) {
        auto sep = token.find(':');
        effect.damagePerEnergyAttached = parseIntOrZero(token.substr(sep + 1));
    } else if (normalizedToken.rfind("randomdmg:", 0) == 0) {
        auto parts = splitAndTrim(token.substr(10), ',');
        if (parts.size() == 2) {
            effect.randomHitDamage = parseIntOrZero(parts[0]);
            effect.randomHitCount = parseIntOrZero(parts[1]);
        }
    } else if (normalizedToken == "poisonopp") {
        effect.poisonOpp = true;
    } else if (normalizedToken == "switchout") {
        effect.switchOutOpp = true;
    } else if (normalizedToken == "bansupporter" || normalizedToken == "bansupporter:nextturn") {
        effect.banSupporter = true;
    } else if (normalizedToken.rfind("dmgifpoisoned:", 0) == 0) {
        effect.extraDmgIfPoisoned = parseIntOrZero(token.substr(14));
    } else if (normalizedToken.rfind("reducedmg:", 0) == 0) {
        effect.damageReduction = parseIntOrZero(token.substr(10));
    } else if (normalizedToken.rfind("bencheddmg:", 0) == 0) {
        effect.benchedDamage = parseIntOrZero(token.substr(11));
    } else if (normalizedToken.rfind("dropenergy:", 0) == 0) {
        skill.energyDrop = parseIntOrZero(token.substr(11));
    } else {
        std::cerr << "Warning: Unrecognized SkillEffect token: " << token << std::endl;
    }
}

bool parsePokemonBlock(std::istream &in, Pokemon &p) {
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (line == "END_POKEMON") return true;

        auto delimPos = line.find(':');
        if (delimPos == std::string::npos) continue;
        std::string key   = trim(line.substr(0, delimPos));
        std::string value = trim(line.substr(delimPos + 1));

        if (key == "Name") {
            p.name = (value == "None" ? "" : value);
        } else if (key == "ex") {
            p.isEx = (toLower(value) == "true");
        } else if (key == "Type") {
            p.type = value;
            if (value == "Supporter")        p.cardType = 1;
            else if (value == "Item")        p.cardType = 2;
            else                             p.cardType = 0;
        } else if (key == "Package") {
            p.package = value;
        } else if (key == "CanEvolve") {
            p.canEvolve = (toLower(value) == "true");
        } else if (key == "CardType") {
            p.cardType = parseIntOrZero(value);
        } else if (key == "HP") {
            p.hp = parseIntOrZero(value);
            p.maxHp = p.hp;
        } else if (key == "Stage") {
            p.stage = parseIntOrZero(value);
            if (p.stage != 0) p.canEvolve = false;
        } else if (key == "Weakness") {
            p.weakness = (value == "None" ? "" : value);
        } else if (key == "RetreatCost") {
            p.retreatCost = parseIntOrZero(value);
        } else if (key == "PrevEvo") {
            p.prevEvo = (value == "None" ? "" : value);
        } else if (key == "NextEvo") {
            p.nextEvo = (value == "None" ? "" : value);
        } else if (key == "Skills") {
            auto skillTokens = splitAndTrim(value, ';');
            for (auto &skillToken : skillTokens) {
                auto tokens = splitAndTrim(skillToken, ',');
                if (tokens.size() < 6) continue;
                Skill s(
                    tokens[0],
                    parseIntOrZero(tokens[1]),
                    parseIntOrZero(tokens[3]),
                    (toLower(tokens[4]) == "true"),
                    parseIntOrZero(tokens[5])
                );
                for (auto &enToken : splitAndTrim(tokens[2], '|')) {
                    auto pos = enToken.find(':');
                    if (pos != std::string::npos) {
                        s.energyRequirements.emplace_back(
                            trim(enToken.substr(0, pos)),
                            parseIntOrZero(trim(enToken.substr(pos + 1)))
                        );
                    }
                }
                p.skills.push_back(std::move(s));
            }
        } else if (key == "SkillEffect") {
            if (p.skills.empty()) continue;
            for (auto &tok : splitAndTrim(value, ';')) {
                applySkillEffectToken(trim(tok), p.skills.back());
            }
        } else if (key == "Abilities") {
            for (auto &tok : splitAndTrim(value, ';')) {
                auto parts = splitAndTrim(tok, '|');
                if (parts.empty()) continue;
                if (toLower(parts[0]) == "none") continue;
                const std::string &name = parts[0];
                const std::string  desc = (parts.size() > 1 ? parts[1] : "");
                p.abilities.emplace_back(name, desc);
            }
        }
    }
    return false;
}

void processEnergyRequirements(std::vector<Skill> &skills) {
    for (size_t i = 0; i < skills.size(); ++i) {
        Skill &skill = skills[i];
        for (auto &req : skill.energyRequirements) {
            if (req.amount <= 0) {
                std::cerr << "Warning: Invalid energy amount for skill: " << skill.skillName << std::endl;
            }
        }
    }
}

void loadCardMapFromFile(
    const std::string &filename,
    std::unordered_map<std::string, Pokemon> &cardMap
) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open card file: " << filename << std::endl;
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (trim(line) == "BEGIN_POKEMON") {
            Pokemon p;
            if (parsePokemonBlock(file, p)) {
                processEnergyRequirements(p.skills);
                cardMap[normalize(p.name)] = p;
            }
        }
    }
    linkEvolutionPointers(cardMap);
}

void loadDeckFromFile(const std::string &filename, std::vector<Pokemon> &deck, const std::unordered_map<std::string, Pokemon> &cardMap) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Error: Could not open file " + filename);
    }

    std::string cardName;
    while (std::getline(file, cardName)) {
        cardName = trim(cardName);
        std::string normalizedCardName = normalize(cardName);
        auto it = cardMap.find(normalizedCardName);
        if (it != cardMap.end()) {
            deck.push_back(it->second);
        } else {
            std::cerr << "Warning: Card \"" << cardName << "\" not found in cardMap. Skipping.\n";
        }
    }
    file.close();
}

void loadCardDatabase(const std::string &filename, std::unordered_map<std::string, Pokemon> &cardMap) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open card database file: " << filename << std::endl;
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty()) continue;

        if (line == "BEGIN_POKEMON") {
            Pokemon p;
            if (parsePokemonBlock(file, p)) {
                processEnergyRequirements(p.skills);
                cardMap[normalize(p.name)] = p;
            }
        }
    }

    linkEvolutionPointers(cardMap);
    file.close();
}
