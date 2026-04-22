#ifndef POKEMONCARD_H
#define POKEMONCARD_H

#include <string>
#include <vector>

struct Skill;
struct Ability;

struct EnergyRequirement {
    std::string energyType;
    int amount;

    EnergyRequirement(const std::string &etype = "", int amt = 0)
        : energyType(etype), amount(amt) {}
};

struct SpecialSkill {
    bool doCoinFlips                = false;
    int  numFlips                   = 0;
    bool flipUntilTails             = false;
    int damagePerFlip               = 0;
    int randomHitDamage             = 0;
    int randomHitCount              = 0;
    int extraDmg                    = 0;
    int extraDmgIfPoisoned          = 0;
    int extraDmgIfParalyzed         = 0;
    int damagePerEnergyAttached     = 0;
    int heal                        = 0;
    int damageReduction             = 0;
    int benchedDamage               = 0;
    int numBenched                  = 0;
    bool switchOutOpp               = false;
    bool paralyzeOpp                = false;
    bool poisonOpp                  = false;
    bool shuffleOpponentBackIfHeads = false;
    bool banSupporter               = false;

    SpecialSkill(
        bool coinFlips = false,
        int flips = 0,
        bool flipTails = false,
        int dmgPerFlipArg = 0,
        int randHits = 0,
        int randHitDmgArg = 0,
        int xtra = 0,
        int xtraIfPoison = 0,
        int xtraIfPara = 0,
        int dmgPerEnergy = 0,
        int healAmt = 0,
        int dmgRed = 0,
        int benchDmg = 0,
        int benchCount = 0,
        bool swOpp = false,
        bool paraOpp = false,
        bool poisOpp = false,
        bool shuffleBack = false,
        bool banSupp = false
    )
      : doCoinFlips(coinFlips)
      , numFlips(flips)
      , flipUntilTails(flipTails)
      , damagePerFlip(dmgPerFlipArg)
      , randomHitDamage(randHitDmgArg)
      , randomHitCount(randHits)
      , extraDmg(xtra)
      , extraDmgIfPoisoned(xtraIfPoison)
      , extraDmgIfParalyzed(xtraIfPara)
      , damagePerEnergyAttached(dmgPerEnergy)
      , heal(healAmt)
      , damageReduction(dmgRed)
      , benchedDamage(benchDmg)
      , numBenched(benchCount)
      , switchOutOpp(swOpp)
      , paralyzeOpp(paraOpp)
      , poisonOpp(poisOpp)
      , shuffleOpponentBackIfHeads(shuffleBack)
      , banSupporter(banSupp)
    {}
};

struct Ability {
    bool activeOnly = false;
    bool poisonOpp = false;
    bool forceSwitchOpp = false;
    bool banSupporter = false;
    bool moveEnergy = false;
    int attachEnergyCount = 0;
    std::string attachEnergyType;
    bool attachOnlyActive = false;

    Ability(
        bool _activeOnly = false,
        bool _poisonOpp = false,
        bool _forceSwitchOpp = false,
        bool _banSupporter = false,
        bool _moveEnergy = false,
        int _attachCount = 0,
        const std::string &_attachType = "",
        bool _attachOnlyActive = false
    )
      : activeOnly(_activeOnly)
      , poisonOpp(_poisonOpp)
      , forceSwitchOpp(_forceSwitchOpp)
      , banSupporter(_banSupporter)
      , moveEnergy(_moveEnergy)
      , attachEnergyCount(_attachCount)
      , attachEnergyType(_attachType)
      , attachOnlyActive(_attachOnlyActive)
    {}

    Ability(const std::string & /*name*/, const std::string & /*description*/)
        : activeOnly(false)
        , poisonOpp(false)
        , forceSwitchOpp(false)
        , banSupporter(false)
        , moveEnergy(false)
        , attachEnergyCount(0)
        , attachEnergyType("")
        , attachOnlyActive(false)
    {}
};

struct Skill {
    std::string skillName;
    int dmg;
    std::vector<EnergyRequirement> energyRequirements;
    int energyDrop;
    bool flipCoin;
    int maxFlip;
    SpecialSkill specialEffect;

    Skill(const std::string &name = "", int damage = 0, int energyDrop = 0,
          bool coin = false, int flipMax = 0)
        : skillName(name)
        , dmg(damage)
        , energyRequirements()
        , energyDrop(energyDrop)
        , flipCoin(coin)
        , maxFlip(flipMax)
        , specialEffect() {}
};

struct Pokemon {
    std::string name;
    bool isEx;
    std::string type;
    std::string package;
    bool canEvolve;
    int cardType;
    int hp;
    int stage;
    std::string prevEvo;
    std::string nextEvo;
    Pokemon *preEvolution;
    Pokemon *postEvolution;
    std::vector<Skill> skills;
    std::string skillEffect;
    std::vector<Ability> abilities;
    std::string weakness;
    int retreatCost;
    std::vector<EnergyRequirement> attachedEnergy;
    bool isPoisoned;
    bool isParalyzed;

    Pokemon(const std::string &n = "", bool ex = false,
            const std::string &t = "", const std::string &pkg = "",
            bool evolve = false, int cType = 0,
            int health = 0, int stg = 0,
            const std::string &weak = "")
      : name(n)
      , isEx(ex)
      , type(t)
      , package(pkg)
      , canEvolve(evolve)
      , cardType(cType)
      , hp(health)
      , stage(stg)
      , prevEvo("")
      , nextEvo("")
      , preEvolution(nullptr)
      , postEvolution(nullptr)
      , skills()
      , skillEffect("")
      , abilities()
      , weakness(weak)
      , retreatCost(0)
      , attachedEnergy()
      , isPoisoned(false)
      , isParalyzed(false)
    {}
};

struct AttackRecord {
    std::string attacker;
    std::string moveName;
    std::string target;
    std::vector<std::string> effects;
};

struct EnergyAttachment {
    std::string pokemonName;
    std::string energyType;
    int amount;
};

struct GameState {
    std::vector<Pokemon> deck;
    std::vector<Pokemon> hand;
    Pokemon activePokemon;
    std::vector<Pokemon> bench;
    int turn;
    bool firstTurn;
    Pokemon opponentActivePokemon;
    std::vector<Pokemon> opponentBench;
    std::vector<std::string> actionHistory;
    std::vector<AttackRecord> attacksThisRound;
    std::vector<EnergyAttachment> yourAttachments;
    std::vector<EnergyAttachment> oppAttachments;
    std::vector<std::string> oppMetaDeckGuesses;

    GameState() : turn(0), firstTurn(true) {}
};

#endif
