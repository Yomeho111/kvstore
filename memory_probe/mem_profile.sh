#!/usr/bin/env bash

# Usage:
#   ./mem_profile.sh <pid> [interval_seconds]
#
# Example:
#   ./mem_profile.sh 12345
#   ./mem_profile.sh 12345 0.5

PID="$1"
INTERVAL="${2:-1}"

if [[ -z "$PID" ]]; then
    echo "Usage: $0 <pid> [interval_seconds]"
    exit 1
fi

if [[ ! -d "/proc/$PID" ]]; then
    echo "Error: process $PID does not exist"
    exit 1
fi

PAGE_SIZE=$(getconf PAGESIZE)

to_mb() {
    # input: KB
    awk -v kb="$1" 'BEGIN { printf "%.2f", kb / 1024 }'
}

pages_to_mb() {
    # input: pages
    awk -v pages="$1" -v ps="$PAGE_SIZE" \
        'BEGIN { printf "%.2f", pages * ps / 1024 / 1024 }'
}

print_header() {
    printf "%-20s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s\n" \
        "TIME" "VmSize" "VmRSS" "RssAnon" "RssFile" "PSS" "Swap" "Data" "Threads"
    printf "%-20s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s\n" \
        "" "(MB)" "(MB)" "(MB)" "(MB)" "(MB)" "(MB)" "(MB)" ""
}

clear
print_header

while true; do
    if [[ ! -d "/proc/$PID" ]]; then
        echo "Process $PID exited."
        exit 0
    fi

    STATUS="/proc/$PID/status"
    STATM="/proc/$PID/statm"
    SMAPS="/proc/$PID/smaps_rollup"

    TIME_NOW=$(date +"%Y-%m-%d %H:%M:%S")

    # From /proc/<pid>/status
    VmSize=$(awk '/^VmSize:/ {print $2}' "$STATUS")
    VmRSS=$(awk '/^VmRSS:/ {print $2}' "$STATUS")
    RssAnon=$(awk '/^RssAnon:/ {print $2}' "$STATUS")
    RssFile=$(awk '/^RssFile:/ {print $2}' "$STATUS")
    VmData=$(awk '/^VmData:/ {print $2}' "$STATUS")
    Threads=$(awk '/^Threads:/ {print $2}' "$STATUS")
    VmSwap=$(awk '/^VmSwap:/ {print $2}' "$STATUS")

    # Fallback defaults
    VmSize=${VmSize:-0}
    VmRSS=${VmRSS:-0}
    RssAnon=${RssAnon:-0}
    RssFile=${RssFile:-0}
    VmData=${VmData:-0}
    VmSwap=${VmSwap:-0}
    Threads=${Threads:-0}

    # PSS from smaps_rollup if available
    if [[ -r "$SMAPS" ]]; then
        Pss=$(awk '/^Pss:/ {print $2}' "$SMAPS")
    else
        Pss=0
    fi

    Pss=${Pss:-0}

    printf "%-20s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s\n" \
        "$TIME_NOW" \
        "$(to_mb "$VmSize")" \
        "$(to_mb "$VmRSS")" \
        "$(to_mb "$RssAnon")" \
        "$(to_mb "$RssFile")" \
        "$(to_mb "$Pss")" \
        "$(to_mb "$VmSwap")" \
        "$(to_mb "$VmData")" \
        "$Threads"

    sleep "$INTERVAL"
done