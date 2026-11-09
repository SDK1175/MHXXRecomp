#!/bin/sh
# Builds build/mhxx-launcher(.exe), the game's launcher (tools/launcher/launcher.cpp). make runs it (its "launcher"
# target); by hand, from the repository folder in MSYS2 UCRT64: sh tools/launcher/build.sh. It goes next to the game
# (MHXX Recomp.exe): the game's Exit to launcher (F1) starts it. For "High-resolution textures" it needs
# mhxx-texextract beside it (make builds that too).
set -e
B=${B:-build}
O=${O:-obj}/launcher
mkdir -p "$B" "$O"
IM=third_party/cimgui/imgui
INC="-Iruntime -Ithird_party/cimgui -I$IM -I$IM/backends"
CXXSRC="tools/launcher/launcher.cpp tools/launcher/title_art.cpp tools/launcher/nxinstall.cpp tools/launcher/texmake.cpp tools/launcher/update.cpp runtime/settings_panel.cpp $IM/imgui.cpp $IM/imgui_draw.cpp $IM/imgui_tables.cpp \
$IM/imgui_widgets.cpp $IM/backends/imgui_impl_sdl3.cpp $IM/backends/imgui_impl_sdlrenderer3.cpp"
CSRC="runtime/config.c runtime/config_store.c runtime/modorder.c runtime/arcmerge.c runtime/nvn/tegra.c runtime/nvn/formats.c runtime/nvn/bcn.c updater/http.c updater/archive.c updater/json.c updater/sha256.c"
OBJS=
for f in $CXXSRC; do
    o="$O/$(basename "$f").o"
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ runtime/settings_locale.h -nt "$o" ]; then
        echo "  $f"
        clang++ -c -O2 -std=c++17 $INC "$f" -o "$o"
    fi
    OBJS="$OBJS $o"
done
for f in $CSRC; do
    o="$O/$(basename "$f").o"
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ runtime/config.h -nt "$o" ]; then
        echo "  $f"
        clang -c -O2 $INC "$f" -o "$o"
    fi
    OBJS="$OBJS $o"
done
case "$(uname -s)" in
MINGW*|MSYS*)
    # (its icon: tools/launcher/icon; llvm-windres, or binutils' windres)
    WINDRES=$(command -v llvm-windres || command -v windres)
    if [ -n "$WINDRES" ]; then
        "$WINDRES" -I tools/launcher/icon -O coff -o "$O/icon.res.o" tools/launcher/icon/mhxx-launcher.rc
        OBJS="$OBJS $O/icon.res.o"
    else
        echo "  (no windres: the launcher is built without its icon)"
    fi
    clang++ -o "$B/mhxx-launcher.exe" $OBJS -fuse-ld=lld -mwindows -lSDL3 -static-libgcc -static-libstdc++ \
        -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic -lole32 -luuid -lshell32 -lwinhttp -Wl,-Bstatic -lz -Wl,-Bdynamic ;;
*) clang++ -o "$B/mhxx-launcher" $OBJS -lSDL3 -lz -lm -ldl -pthread ;;
esac
echo "built: $B/mhxx-launcher"
