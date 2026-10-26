#!/bin/sh
# Extract the launcher icon from your own Geometry Wars 3 APK into icon.jpg (256x256, what NROs use).
# usage: tools/make_icon.sh path/to/gw3.apk
set -e
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
unzip -oj -q "$1" res/drawable-xxxhdpi-v4/game.png -d "$t"
python3 -c "from PIL import Image; Image.open('$t/game.png').convert('RGB').resize((256,256), Image.LANCZOS).save('$(dirname "$0")/../icon.jpg', quality=95)"
echo "wrote icon.jpg"
