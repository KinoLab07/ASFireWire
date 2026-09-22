#!/usr/bin/env bash
# Compila FIRMANDO y despliega en la ruta que el sistema tiene registrada.
#
# build.sh compila con CODE_SIGNING_ALLOWED=NO a proposito: sirve para comprobar
# que el codigo compila, no para instalar. sysextd rechaza un dext sin firma con
# el error -67062 y el mensaje "cannot allow apps outside /Applications", asi que
# una app construida con build.sh a secas nunca llega a instalarse.
#
# El otro motivo de este script: build.sh escribe en ./build/DerivedData, pero la
# extension registrada apunta a OTRO bundle. Tenerlos separados hizo que durante un
# buen rato se probara una app distinta de la que se compilaba.
#
# El destino era el DerivedData de Xcode, con un hash de la ruta del proyecto escrito
# a mano. Eso se rompio al mover el repositorio: el hash dejo de corresponder. Ahora
# el destino es /Applications, que es de donde macOS tiene registrada la extension
# y a donde copia tambien el install.sh de las releases. No depende de donde este
# el repositorio.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

: "${DEVELOPER_DIR:=/Applications/Xcode-27.0.0.app/Contents/Developer}"
export DEVELOPER_DIR

SRC="build/DerivedData/Build/Products/Debug/ASFW.app"
DEST="/Applications/ASFW.app"

./build.sh "$@" --set CODE_SIGNING_ALLOWED=YES --set CODE_SIGNING_REQUIRED=YES --set CODE_SIGN_IDENTITY=-

DEXT="$SRC/Contents/Library/SystemExtensions/net.mrmidi.ASFW.ASFWDriver.dext"
SIG="$(codesign -dv "$DEXT" 2>&1 || true)"
case "$SIG" in
  *"Signature="*) ;;
  *) echo "ERROR: el dext salio sin firmar; sysextd lo rechazaria con -67062"; echo "$SIG"; exit 1;;
esac

pkill -f "ASFW.app/Contents/MacOS/ASFW" 2>/dev/null || true
sleep 2
rm -rf "$DEST"
ditto "$SRC" "$DEST"
open "$DEST"

echo
echo "desplegada version $(/usr/libexec/PlistBuddy -c 'Print CFBundleVersion' "$DEST/Contents/Info.plist")"
echo "instalada ahora:  $(systemextensionsctl list | grep -o '0\.3\.0/[0-9]*' | tail -1)"
echo "si el numero de version cambio, dale a Install en la app."
