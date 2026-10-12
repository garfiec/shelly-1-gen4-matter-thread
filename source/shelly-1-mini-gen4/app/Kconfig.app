# Shared by every Shelly 1 Mini Gen4 product line (main/Kconfig.projbuild
# sources it).

menu "Example Configuration"

    config ENABLE_MEMORY_PROFILING
        bool "Enable Memory Profiling"
        default n
        help
            Enable this option to include memory profiling features in the example.
            This will allow you to monitor memory usage during runtime.

endmenu

menu "Shelly board"

    # Shelly 1 Mini Gen4 pin map, PCB v0.1.2. See docs/GPIO.md.

    config SHELLY_HARDWARE_CODE
        string "Shelly hardware code"
        default "Mini1G4"
        help
            Shelly's own code for this hardware, as in make-webui-ota-zip.py.
            Stored in every image (shelly_hw_desc.h) so Wi-Fi maintenance mode
            refuses firmware built for a different Shelly model.

    config SHELLY_RELAY_GPIO
        int "Relay GPIO (active high)"
        default 10

    config SHELLY_LED_GPIO
        int "Status LED GPIO"
        default 5

    config SHELLY_LED_ACTIVE_LOW
        bool "Status LED is active low"
        default y

    config SHELLY_BUTTON_GPIO
        int "Button GPIO"
        default 22

    config SHELLY_BUTTON_ACTIVE_LOW
        bool "Button is active low"
        default y

    config SHELLY_SWITCH_GPIO
        int "SW terminal GPIO"
        default 12

endmenu
