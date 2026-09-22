# Harnais de test autonomes de mod-ascension-compat

Chaque sous-repertoire porte un `run.py` qui compile le VRAI `.cpp` du module (ou
une tranche d'AzerothCore) contre des stubs, puis execute des assertions. Aucun
serveur, aucune base, aucun client : un harnais s'execute seul, en quelques
secondes.

```bash
python3 tests/<nom>/run.py          # un harnais
```

## Code de sortie

| Code | Sens |
|---|---|
| `0` | reussite |
| `77` | **SKIP** — une dependance externe manque sur cette machine, le harnais n'a rien teste |
| autre | echec reel : le code teste ou le harnais lui-meme est en cause |

Un harnais saute imprime `SKIP: <raison>`. Il ne doit **jamais** compter comme un
succes : c'est exactement ce que `77` evite.

## Variables d'environnement

| Variable | Defaut | Role |
|---|---|---|
| `COA_DBC_DIR` | `/opt/coa/server/data/dbc` | repertoire des `.dbc` lus par les harnais |
| `COA_DATAMINE_DIR` | — | datamine Ascension (`raw/tables/...`), absent de cette machine |
| `CXX` | `g++`, sinon `c++` | compilateur C++ sous Linux |
| `VCToolsInstallDir` | — | si posee **et** que `cl.exe` s'y trouve, MSVC est prefere |

Tout cela passe par `coa_test_env.py`, a la racine de `tests/` : `dbc_dir()`,
`datamine_dir()`, `compile_cxx()`, `require_helper()`, `skip()`. Un harnais neuf
s'en sert plutot que de recoder une detection de plateforme.

## Etat mesure le 2026-09-22 (P-012)

71 harnais : **42 passent, 29 sautent, 0 echouent**.

Historique : 8 passaient et 63 echouaient avant la reprise du 2026-09-21 ; 30/29/12
apres la premiere passe ; 42/29/0 depuis que les douze fixtures en retard ont ete
rattrapees. Mesure : `for d in tests/*/run.py; do python3 -B "$d"; done`, codes de
retour comptes.

### Les 29 SKIP — dependances absentes, rien a corriger dans le module

- **27 harnais** importent un helper `/opt/coa/tools/Test-*.py` qui n'a jamais ete
  livre dans ce depot ni sur la machine de service (`Test-LocalLoginCollections.py`,
  `Test-BarbarianCompletion.py`, `Test-WitchDoctorCompletion.py`,
  `Test-NecromancerCompletion.py`, `Test-StarcallerCompletion.py`,
  `Test-TemplarCompletion.py`, `Test-WitchHunterCompletion.py`,
  `Test-KnightOfXorothCompletion.py`, `Test-AdditionalTargetContracts.py`).
  Les rendre executables suppose de retrouver ou de reecrire ces fixtures.
  Tant qu'ils sautent, **leur code d'apres-skip n'est plus compile du tout** :
  une derive de fixture qui s'y installerait resterait invisible.
- **2 harnais** (`greater_imp`, `secondary_appearances`) exigent le datamine
  Ascension, absent : poser `COA_DATAMINE_DIR`.

### Les douze echecs de la veille — ce qui les causait vraiment

Ils compilaient le vrai `.cpp` contre une fixture en retard. **Onze etaient des
erreurs de compilation** : un membre, une constante, une surcharge ou une classe de
base que le module utilise et que la fixture ne declarait pas. Le douzieme,
`taught_abilities`, echouait sur une **assertion de contenu** (`len(replacements) == 13`)
et non a la compilation : ce cas-la a ete tranche en lisant **les deux cotes** —
`AscensionTalentReplacementData.h` porte aujourd'hui 16 entrees, et la croissance
est deliberee (commit `a5abbb1`, « Fait apparaitre dix boutons de remplacement qui
n'apparaissaient jamais »). C'est bien la fixture qui etait en retard, mais ce
constat a ete **etabli**, pas presume.

La regle a en tirer : une erreur de compilation contre la fixture met en cause la
fixture ; **une assertion de contenu qui echoue ne dit pas qui a tort** et se
tranche en lisant le module et le harnais.

Corrections apportees, par nature :

- membre de synchronisation absent de la fixture (`mutable std::mutex _stateLock`,
  miroir de `AscensionCompat.cpp:2195`) : `taught_abilities` ;
- surcharge manquante (`Unit::HasAura(uint32)`, `AuraScript::GetSpellInfo()`,
  `SpellScript::GetHitUnit()`, `Player::GetTotalAuraMultiplier` a predicat) :
  `runemaster_secondary`, `chronomancer_movement`, `manastorm` ;
- superposition de fixtures qui se redeclaraient (`runemaster_hurricane`) ;
- `constexpr double M_PI` ecrase par la MACRO de `<cmath>` — la ligne entiere
  disparait, et ce qui la partage avec elle : `runemaster_rift_clones`, `flesh_hook` ;
- API de jeu nouvellement appelee par le module et absente de la fixture
  (`Unit::RemoveAllMinionsByEntry`, `PlayerScript::OnPlayerHasActivePowerType`,
  `Spell::m_targets`, `Loot::FillLoot`/`GetMaxSlotInLootFor`/`LootItemInSlot`,
  `Player::CanStoreNewItem`/`StoreNewItem`/`DestroyItemCount`) : `animated_blood`,
  `primalist_secondary`, `stormbringer_resources`, `adventurer_cache` ;
- fonctionnalite entiere non modelisee (`rulesets` : la sanction High Risk).

Chaque nom de fonction, de champ et de constante ecrit dans une fixture porte en
commentaire le fichier et la ligne du moteur d'ou il a ete **lu**.

`adventurer_cache` a demande davantage qu'un rattrapage : le module ne passe plus
par une fenetre de butin (`SendLoot`), il verse la recompense dans les sacs. Les
assertions qui comptaient les ouvertures de fenetre ont ete remplacees par des
assertions sur ce qui est detruit, stocke et annonce.

`rulesets` est le seul filet automatisable de la sanction High Risk
(`AscensionRulesets.cpp`). Sa fixture couvre desormais `PenaltyApplies`,
`Premium`, `MoneyText`, le cycle complet `Remember` / `Arm` / `Tick` / `Settle`
(prime payee, piece remise au tueur, piece envoyee par courrier, exemption
d'ecart de niveau, dette oubliee, mort en PvE), le drapeau FFA et le filet du
cadavre a vie non nulle.

## Ce que la suite ne garantit pas

- **Les donnees sont vivantes.** Par defaut les harnais lisent les DBC de
  production (`/opt/coa/server/data/dbc`) et font des assertions sur des lignes de
  `Spell.dbc`. La lecture est seule, sans risque pour le serveur, mais une
  modification de gameplay dans les DBC fera basculer des harnais sans qu'aucune
  ligne de code n'ait change. `COA_DBC_DIR` permet de s'en isoler ; ce n'est pas
  fait par defaut.
- **Les 29 SKIP ne compilent rien.**

## Integration continue

`.github/workflows/quality.yml:20` execute deja **un** harnais de ce repertoire :

```yaml
      - run: python -B modules/mod-ascension-compat/tests/client_compat/run.py
```

`client_compat` n'importe pas `coa_test_env.py` et ne saute jamais, donc rien ne
casse aujourd'hui. Mais **GitHub Actions traite tout code de retour non nul comme
un echec, 77 compris** : brancher un harnais susceptible de sauter sur ce workflow
casserait la CI sur une machine sans la dependance. Envelopper l'appel :

```yaml
      - run: python -B modules/mod-ascension-compat/tests/<nom>/run.py; rc=$?; [ $rc -eq 77 ] || exit $rc
```

## Indexation — `coa_test_env.py` et ce README vont avec les `run.py`

Les 61 `run.py` qui font `from coa_test_env import ...` **ne fonctionnent pas sans
`coa_test_env.py`**. Ce fichier et ce README doivent etre indexes **dans le meme
commit** que les `run.py` qui en dependent : un commit partiel casse 61 harnais
d'un coup par `ModuleNotFoundError`.

`tools/check_repository.py` les voit : il enumere par
`git ls-files --cached --others --exclude-standard`, donc les fichiers non suivis
non ignores aussi. Le relancer apres indexation reste la bonne habitude.
