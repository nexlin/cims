#!/bin/bash
#------------------------------------------------------------------------------
# csp start/stop script
#------------------------------------------------------------------------------

# Get the absolute path of the script's directory
bin_dir=$(cd "$(dirname "$0")" && pwd)
root_dir=$(cd "$bin_dir/.." && pwd)
this_script="$bin_dir/$(basename "$0")"

echo "RootDir: $root_dir"

log_dir="$root_dir/log"
cfg_dir="$root_dir/config"

program="csp"
program_list="$program"
program_arg="$cfg_dir/csp.json"
program_name="csp"
logfile="$log_dir/cspdog.log"

# get process pid
getpids(){
    if [ "$1" = "csp" ]; then
        pgrep -x "$1"
    else
        pgrep -f "$1"
    fi
}

start_program(){
    echo "Starting $program..."
    ulimit -c unlimited
    ulimit -n 65535
    
    cd "$bin_dir"
    "$bin_dir/$program" "$program_arg"
}

start_program_input(){
    d=`date +%Y/%m/%d_%H:%M:%S`
    echo "[$d] $1: Starting..." >> "$logfile"
    ulimit -c unlimited
    ulimit -n 65535
    cd "$bin_dir"
    "$bin_dir/$program" "$program_arg"
}

start() {
    if [ -n "$(getpids $program)" ]; then
        echo "$program_name: already running. Stopping first..."
        stop
        sleep 2
    fi
    
    echo "$program_name: Starting..."
    start_program_input "$program_name"
}

stop() {
    echo "$program_name : Stopping... "
    pids=$(getpids $program)
    if [ -n "$pids" ]; then
        kill $pids
        echo "Sent kill signal to $pids. Waiting..."
        sleep 2
        
        # Check if still running and force kill if needed
        pids=$(getpids $program)
        if [ -n "$pids" ]; then
            echo "Processes still running: $pids. Killing with -9..."
            kill -9 $pids
        fi
    else
        echo "No process found to stop"
    fi
}

status() {
    pids=$(getpids $program)
    if [ -n "$pids" ]; then
        echo "$program_name: running ($pids)"
    else
        echo "$program_name: not running"
    fi

    pids=$(getpids "$(basename "$this_script") watchdog")
    if [ -n "$pids" ]; then
        echo "watchdog: running ($pids)"
    else
        echo "watchdog: not running"
    fi
}

watchdog(){
    echo "watchdog: Starting..."
    sleep 10
    while true; do
        for program in $program_list
        do
            if [ -f "$bin_dir/$program" ]; then
                if [ -z "$(getpids $program)" ]; then
                    start_program_input "$program_name"
                fi
            else
                echo "[error] $bin_dir/$program does not exist."
            fi
        done
        sleep 10
    done
}

watchdog_start(){
    watchdog_stop
    nohup "$this_script" watchdog > /dev/null 2>&1 &
    echo "watchdog started."
}

watchdog_stop(){
    pids=$(getpids "$(basename "$this_script") watchdog")
    if [ -n "$pids" ]; then
        kill $pids
        echo "watchdog : Stopped"
    fi
}

# Main
case "$1" in
    start)
        if [ "$2" = "-f" ]; then
            # Foreground mode
            start_program
        else
            start
            watchdog_start
        fi
        ;;
    stop)
        watchdog_stop
        stop
        ;;
    status)
        status
        ;;
    restart)
        watchdog_stop
        stop
        sleep 1
        start
        watchdog_start
        ;;
    watchdog)
        watchdog
        ;;
    *)
        echo "Usage: $0 {start [-f]|stop|status|restart}"
        exit 1
        ;;
esac
