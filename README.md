# nickname_cleaner (Plugify)

Nickname cleaner, ported from the Metamod plugin nickname_cleaner (ABKAM). Reads addons/configs/nickname_cleaner/settings.ini (DefaultName) plus whitelist.txt/blocklist.txt and rewrites the name of every human player whose nickname contains a blocked word or an advertisement (blocklist entries '*.xx' match the end of a token, other entries a substring; the whitelist wins; http://, https:// and www. are always dropped) on round_prestart, one second after player_spawn and on client authorization. A name whose tokens are all filtered becomes DefaultName. No exported methods.

Plugify port (fork) of **nickname_cleaner** by ABKAM (Metamod (C++)).
Original: https://github.com/ABKAM2023/cs2-nickname_cleaner
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, hudmenu (optional)

## Installation
Download `nickname_cleaner-1.0.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/nickname_cleaner`.

## License
GPL-3.0, see LICENSE.

