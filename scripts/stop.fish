#!/usr/bin/env fish
# Cleanly stop all three Hercules servers.
# Usage: ./scripts/stop.fish

set -l session hercules

if not tmux has-session -t $session 2>/dev/null
    echo "No tmux session named '$session' is running."
    exit 0
end

echo "Stopping Hercules servers in reverse order (map → char → login)..."

# Send Ctrl+C to map-server first (reverse of start order)
tmux send-keys -t $session:map C-c
sleep 1

tmux send-keys -t $session:char C-c
sleep 1

tmux send-keys -t $session:login C-c
sleep 2

# Give them a moment to flush logs and close DB connections cleanly
echo "Waiting for clean shutdown..."
sleep 3

# Kill the tmux session
tmux kill-session -t $session
echo "Done. Session '$session' terminated."

