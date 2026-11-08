#!/bin/sh
# Builds build/mhxx-launcher(.exe), the game's launcher (tools/launcher/launcher.cpp). Run from the repository folder,
# in MSYS2 UCRT64 on Windows: sh tools/launcher/build.sh. It goes next to mhgurecomp: the game's Exit to launcher
# (F1) starts it.
set -e
B=${B:-build}
O=${O:-obj}/launcher
mkdir -p "$B" "$O"
IM=third_party/cimgui/imgui
INC="-Iruntime -Ithird_party/cimgui -I$IM -I$IM/backends"
CXXSRC="tools/launcher/launcher.cpp runtime/settings_panel.cpp $IM/imgui.cpp $IM/imgui_draw.cpp $IM/imgui_tables.cpp \
$IM/imgui_widgets.cpp $IM/backends/imgui_impl_sdl3.cpp $IM/backends/imgui_impl_sdlrenderer3.cpp"
CSRC="runtime/config.c runtime/config_store.c runtime/modorder.c runtime/arcmerge.c"
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
    clang++ -o "$B/mhxx-launcher.exe" $OBJS -fuse-ld=lld -mwindows -lSDL3 -static-libgcc -static-libstdc++ \
        -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic -lole32 -luuid -lshell32 ;;
*) clang++ -o "$B/mhxx-launcher" $OBJS -lSDL3 -lm -pthread ;;
esac
echo "built: $B/mhxx-launcher"
