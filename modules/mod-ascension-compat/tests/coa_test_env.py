"""Environnement d'execution partage des harnais autonomes de mod-ascension-compat.

Les harnais de ce repertoire compilent le VRAI .cpp du module contre des stubs et
verifient son comportement, sans serveur ni base. Ils ont ete ecrits sur un poste
Windows : compilateur MSVC lu dans `VCToolsInstallDir`, DBC cherches dans un
`runtime/server/data/dbc` qui n'existe pas ici. Sur la machine de service, 63 des
71 echouaient avant d'avoir compile quoi que ce soit (P-012).

Ce module porte les trois services qui manquaient, un par cause d'echec :

  dbc_dir()      ou sont les DBC, sans chemin en dur ;
  compile_cxx()  compile avec le compilateur de la plateforme (MSVC ou g++) ;
  skip()         sortie propre en SKIP quand une dependance externe manque.

Variables d'environnement lues :
  COA_DBC_DIR   repertoire des .dbc          (defaut /opt/coa/server/data/dbc)
  CXX           compilateur C++ a utiliser   (defaut g++, puis c++)
  VCToolsInstallDir  si presente et si cl.exe s'y trouve, MSVC est prefere.
"""

import os
import shutil
import subprocess
from pathlib import Path

# Convention automake, reprise par ctest et meson : ni succes ni echec, "non execute".
SKIP_EXIT_CODE = 77

DEFAULT_DBC_DIR = Path('/opt/coa/server/data/dbc')


def skip(reason):
    """Sort du harnais en SKIP. Un harnais saute ne doit jamais passer pour un succes."""
    print(f'SKIP: {reason}')
    raise SystemExit(SKIP_EXIT_CODE)


def dbc_dir(required='Spell.dbc'):
    """Repertoire des DBC. COA_DBC_DIR si posee, sinon l'emplacement de service."""
    raw = os.environ.get('COA_DBC_DIR')
    directory = Path(raw).expanduser() if raw else DEFAULT_DBC_DIR
    if required and not (directory / required).is_file():
        skip(f'{required} introuvable dans {directory} — poser COA_DBC_DIR')
    return directory


def require_helper(path):
    """Chemin d'un helper externe, ou SKIP s'il n'a jamais ete livre sur cette machine.

    Vingt-cinq harnais importent un `/opt/coa/tools/Test-*.py` qui n'existe pas dans
    ce depot ni sur la machine de service. Echouer ferait croire a une regression du
    module ; passer en silence ferait croire a un test vert. On sort en SKIP.
    """
    path = Path(path)
    if not path.is_file():
        skip(f'helper externe absent : {path} (jamais livre — voir P-012)')
    return path


def datamine_dir(required=None, default=None):
    """Repertoire du datamine Ascension, ou SKIP s'il n'est pas sur cette machine.

    COA_DATAMINE_DIR l'emporte ; sinon `default`, l'emplacement que le harnais
    attendait. `required` est un chemin relatif — fichier ou repertoire — dont la
    presence sert de preuve que le datamine est bien celui-la.
    """
    raw = os.environ.get('COA_DATAMINE_DIR')
    directory = Path(raw).expanduser() if raw else (Path(default) if default else None)
    if directory is None:
        skip('datamine absent — poser COA_DATAMINE_DIR')
    if not directory.is_dir():
        skip(f'datamine absent : {directory} — poser COA_DATAMINE_DIR')
    if required and not (directory / required).exists():
        skip(f'{required} introuvable dans {directory} — poser COA_DATAMINE_DIR')
    return directory


def msvc_compiler():
    """cl.exe si l'environnement MSVC est reellement la, sinon None."""
    root = os.environ.get('VCToolsInstallDir')
    if not root:
        return None
    candidate = Path(root) / 'bin/Hostx64/x64/cl.exe'
    return candidate if candidate.is_file() else None


def posix_compiler():
    """g++ (ou CXX, ou c++) si l'un d'eux est installe, sinon None."""
    override = os.environ.get('CXX')
    if override:
        return shutil.which(override) or (override if Path(override).is_file() else None)
    return shutil.which('g++') or shutil.which('c++')


# Avertissements que g++ produit et que MSVC /W4 ne produit pas. Les laisser fatals
# ferait echouer sous g++ des harnais que /W4 /WX acceptait, sans rien dire du code
# teste. On n'en eteint que trois, nommement, pour garder le reste du filet :
#   missing-field-initializers  les fixtures initialisent volontairement en partie ;
#   unknown-pragmas             le code du module porte des `#pragma warning` MSVC ;
#   misleading-indentation      les harnais decoupent des fonctions d'AzerothCore au
#                               milieu du corps, ce qui deplace l'indentation.
POSIX_WARNING_OPT_OUT = (
    '-Wno-missing-field-initializers',
    '-Wno-unknown-pragmas',
    '-Wno-misleading-indentation',
)


def cxx_command(source, exe, include_dirs=(), warnings_as_errors=True,
                std='c++20', msvc_extra=(), posix_extra=()):
    """Ligne de commande de compilation, traduite pour le compilateur present.

    Equivalences MSVC -> g++ : /std:c++20 -> -std=c++20, /W4 -> -Wall -Wextra,
    /WX -> -Werror, /Fe<exe> -> -o <exe>. /nologo, /EHsc et /utf-8 n'ont pas
    d'equivalent utile sous g++ (exceptions et UTF-8 y sont deja le defaut).
    """
    compiler = msvc_compiler()
    if compiler is not None:
        argv = [str(compiler), '/nologo', f'/std:{std}', '/EHsc', '/W4', '/utf-8']
        if warnings_as_errors:
            argv.append('/WX')
        argv += list(msvc_extra)
        argv += [f'/I{directory}' for directory in include_dirs]
        argv += [str(source), '/Fe' + str(exe)]
        return argv
    compiler = posix_compiler()
    if compiler is None:
        skip('aucun compilateur C++ trouve (ni VCToolsInstallDir/cl.exe, ni g++)')
    argv = [str(compiler), f'-std={std}', '-Wall', '-Wextra', *POSIX_WARNING_OPT_OUT]
    if warnings_as_errors:
        argv.append('-Werror')
    argv += list(posix_extra)
    argv += [f'-I{directory}' for directory in include_dirs]
    argv += [str(source), '-o', str(exe)]
    return argv


def compile_cxx(source, exe, cwd=None, timeout=120, include_dirs=(), warnings_as_errors=True,
                std='c++20', msvc_extra=(), posix_extra=()):
    """Compile `source` en `exe` et rend le chemin de l'executable produit."""
    argv = cxx_command(source, exe, include_dirs, warnings_as_errors, std, msvc_extra, posix_extra)
    subprocess.run(argv, cwd=None if cwd is None else str(cwd), check=True, timeout=timeout)
    return Path(exe)
