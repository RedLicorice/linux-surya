#!/bin/sh
# claude-powerlog: battery and CPU log for the idle-power measurements.
# Every INTERVAL s one CSV row; every SNAP_EVERY rows a snapshot of per-cluster
# time_in_state and cpuidle residency (cumulative counters, diff them later).
# Rows are written plugged and unplugged; the `online` column tells them apart.
OUT=${POWERLOG_DIR:-$HOME/.local/state/claude-powerlog}
INTERVAL=${POWERLOG_INTERVAL:-30}
SNAP_EVERY=${POWERLOG_SNAP_EVERY:-20}
BAT=/sys/class/power_supply/qcom_qg
CHG=/sys/class/power_supply/pm8150b-charger
BL=/sys/class/backlight/backlight
mkdir -p "$OUT"
csv=$OUT/power.csv
[ -f "$csv" ] || echo "ts,uptime,online,capacity,current_now_ua,current_avg_ua,voltage_now_uv,voltage_avg_uv,temp_dC,bl_power,brightness,cpu0_khz,cpu6_khz,load1,psi_cpu_some10,suspends,wifi" > "$csv"
r() { cat "$1" 2>/dev/null || echo NA; }
n=0
while :; do
    psi=$(sed -n 's/^some avg10=\([0-9.]*\).*/\1/p' /proc/pressure/cpu 2>/dev/null)
    echo "$(date +%s),$(cut -d' ' -f1 /proc/uptime),$(r $CHG/online),$(r $BAT/capacity),$(r $BAT/current_now),$(r $BAT/current_avg),$(r $BAT/voltage_now),$(r $BAT/voltage_avg),$(r $BAT/temp),$(r $BL/bl_power),$(r $BL/brightness),$(r /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq),$(r /sys/devices/system/cpu/cpufreq/policy6/scaling_cur_freq),$(cut -d' ' -f1 /proc/loadavg),${psi:-NA},$(r /sys/power/suspend_stats/success),$(r /sys/class/net/wld0/operstate)" >> "$csv"
    if [ $((n % SNAP_EVERY)) -eq 0 ]; then
        {
            echo "=== $(date +%s) online=$(r $CHG/online)"
            for p in 0 6; do
                echo "policy$p time_in_state (kHz 10ms):"
                cat /sys/devices/system/cpu/cpufreq/policy$p/stats/time_in_state
            done
            for c in 0 6; do
                for s in /sys/devices/system/cpu/cpu$c/cpuidle/state*; do
                    echo "cpu$c $(cat $s/name) usage=$(cat $s/usage) time_us=$(cat $s/time)"
                done
            done
        } >> "$OUT/snapshots.txt"
    fi
    n=$((n + 1))
    sleep "$INTERVAL"
done
