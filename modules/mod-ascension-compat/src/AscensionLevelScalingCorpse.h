#ifndef ASCENSION_LEVEL_SCALING_CORPSE_H
#define ASCENSION_LEVEL_SCALING_CORPSE_H

#include "Creature.h"
#include "DataMap.h"

// Depecage (26/10) : a la mort, le cadavre d'une creature a depecer reprend son niveau d'origine
// (AscensionCompatLevelScalingScript::RestoreOriginalLevelOnDeath, AscensionCompat.cpp). Le niveau
// auquel elle a ete COMBATTUE reste lisible ici, pour ce qui juge le combat et non le depecage
// (Lame de trophee : refus d'une creature grise). Reecrit a chaque mort d'une creature mise a
// l'echelle, pour qu'une mort non hissee ne relise jamais la valeur d'une vie precedente.
struct AscensionNiveauAuCombat final : public DataMap::Base
{
  uint8 Niveau = 0;
};

inline constexpr char const* CLE_NIVEAU_AU_COMBAT = "AscensionNiveauAuCombat";

// Niveau de la creature pendant son dernier combat : celui d'avant la remise a l'origine, sinon le niveau actuel.
inline uint8 NiveauAuCombat(Creature const* creature)
{
  if (!creature->IsAlive())
    if (AscensionNiveauAuCombat const* n = creature->CustomData.Get<AscensionNiveauAuCombat>(CLE_NIVEAU_AU_COMBAT))
      if (n->Niveau)
        return n->Niveau;
  return creature->GetLevel();
}

#endif
