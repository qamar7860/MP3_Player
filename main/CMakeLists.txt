# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.

idf_component_register(
    SRCS
        "main.c"
        "Display/Display.c"
        "UI.c"
        "Player.c"
        "Bluetooth.c"
        "SD_Bench.c"
        "minimp3.h"
        "SD_Card.c"
        "Battery.c"
        "Temperature.c"
        "BG.c"
    INCLUDE_DIRS
        "."
        "Display"
)

