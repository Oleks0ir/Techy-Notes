#!/bin/sh
# The uninstaller's file list (see tools/installer.nsi.in): a Delete line per file of the deployed program folder $1,
# then an RMDir line per folder, deepest first. Usage: tools/uninstall-list.sh Techy-Notes > uninstall-list.nsh
cd "$1" || exit 1
find . -type f | sed 's|^\./||' | while IFS= read -r f; do printf 'Delete "$INSTDIR\\%s"\n' "$(printf %s "$f" | tr / '\\\\')"; done
find . -mindepth 1 -depth -type d | sed 's|^\./||' | while IFS= read -r d; do printf 'RMDir "$INSTDIR\\%s"\n' "$(printf %s "$d" | tr / '\\\\')"; done
