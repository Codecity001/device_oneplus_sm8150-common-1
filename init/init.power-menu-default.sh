#!/system/bin/sh
# The Pixel Setup Wizard forces power_button_long_press=5 (assistant) on a clean
# flash; reapply the power menu once after setup, then mark it one-shot.
if [ "$(settings get global power_menu_default_applied)" = "1" ]; then
    exit 0
fi

attempts=0
while [ "$attempts" -lt 120 ]; do
    if [ "$(settings get secure user_setup_complete)" = "1" ]; then
        settings put global power_button_long_press 1
        settings put global power_menu_default_applied 1
        exit 0
    fi
    sleep 5
    attempts=$((attempts + 1))
done
