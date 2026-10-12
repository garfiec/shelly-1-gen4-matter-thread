# Shared by every Shelly 1PM Gen4 product line (main/Kconfig.projbuild sources it).

menu "Example Configuration"

    config ENABLE_MEMORY_PROFILING
        bool "Enable Memory Profiling"
        default n
        help
            Enable this option to include memory profiling features in the example.
            This will allow you to monitor memory usage during runtime.

endmenu

menu "Shelly board"

    # Shelly 1PM Gen4 pin map, PCB v0.1.2. See docs/GPIO.md. Mapped on one
    # unit measured at 120 V / 60 Hz; confirm the PCB revision before
    # flashing (docs/FLASHING.md).

    config SHELLY_HARDWARE_CODE
        string "Shelly hardware code"
        default "S1PMG4"
        help
            Shelly's own code for this hardware, as in make-webui-ota-zip.py.
            Stored in every image (shelly_hw_desc.h) so Wi-Fi maintenance mode
            refuses firmware built for a different Shelly model.

    config SHELLY_RELAY_GPIO
        int "Relay GPIO (active high)"
        default 4

    config SHELLY_LED_GPIO
        int "Status LED GPIO"
        default 11

    config SHELLY_LED_ACTIVE_LOW
        bool "Status LED is active low"
        default y

    config SHELLY_BUTTON_GPIO
        int "Button GPIO"
        default 1

    config SHELLY_BUTTON_ACTIVE_LOW
        bool "Button is active low"
        default y

    config SHELLY_SWITCH_GPIO
        int "SW terminal GPIO"
        default 10

    config SHELLY_HAS_NTC
        bool "Board NTC fitted"
        default y

    config SHELLY_NTC_GPIO
        int "NTC ADC GPIO"
        depends on SHELLY_HAS_NTC
        default 3

    config SHELLY_NTC_R_FIXED_OHM
        int "NTC divider resistor, supply side (ohm)"
        depends on SHELLY_HAS_NTC
        default 10000

    config SHELLY_NTC_R25_OHM
        int "NTC resistance at 25 °C (ohm)"
        depends on SHELLY_HAS_NTC
        default 10000

    config SHELLY_NTC_BETA
        int "NTC beta"
        depends on SHELLY_HAS_NTC
        default 3350
        help
            Estimated, not measured. The trip thresholds leave margin for it.

    config SHELLY_NTC_VREF_MV
        int "NTC divider supply (mV)"
        depends on SHELLY_HAS_NTC
        default 3300

    config SHELLY_HAS_BL0942
        bool "BL0942 power meter fitted"
        default y
        select SHELLY_HAS_METER

    config SHELLY_BL0942_UART_NUM
        int "BL0942 UART"
        depends on SHELLY_HAS_BL0942
        default 1

    config SHELLY_BL0942_TX_GPIO
        int "BL0942 TX GPIO (ESP32 to meter)"
        depends on SHELLY_HAS_BL0942
        default 6

    config SHELLY_BL0942_RX_GPIO
        int "BL0942 RX GPIO (meter to ESP32)"
        depends on SHELLY_HAS_BL0942
        default 7

    config SHELLY_BL0942_LINE_60HZ
        bool "Mains is 60 Hz (corrected at boot from the measured frequency)"
        depends on SHELLY_HAS_BL0942
        default y

    config SHELLY_BL0942_VOLTAGE_REF
        string "BL0942 voltage reference (counts per V)"
        depends on SHELLY_HAS_BL0942
        default "14462.09548"
        help
            Measured on a Shelly 1PM Gen4 at 120 V / 60 Hz
            (automatous-io/shelly-gen4-esphome). Power and energy scaling
            derive from this and the current reference.

    config SHELLY_BL0942_CURRENT_REF
        string "BL0942 current reference (counts per A)"
        depends on SHELLY_HAS_BL0942
        default "253772.51527"

    config SHELLY_BL0942_CURRENT_FULL_SCALE_MA
        int "BL0942 current channel full scale (mA rms)"
        depends on SHELLY_HAS_BL0942
        default 29700
        help
            The current channel clips at 30 mV rms; with this board's shunt
            (about 1.01 mV/A) that is 29.7 A for a sine wave. Readings at
            this level are treated as clipped.

endmenu
