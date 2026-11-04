#!/system/bin/sh
# nuwa-618 is being removed: nothing of it stays outside its directory but the switches' files in
# /data/adb, which are the user's choices for the next install. The kernel keeps its own defaults
# from the next boot on.
sh "${0%/*}/trial-guard.sh" _clear
exit 0
