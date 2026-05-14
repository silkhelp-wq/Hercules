#!/usr/bin/env fish
# Start all three Hercules servers in a tmux session.
# Usage: ./scripts/start.fish

set -l session hercules
set -l herc_dir (realpath (dirname (status -f))/..)

# Refuse to start if a session already exists
if tmux has-session -t $session 2>/dev/null
    echo "tmux session '$session' is already running."
    echo "Attach with:  tmux attach -t $session"
    echo "Or stop with: ./scripts/stop.fish"
    exit 1
end

echo "Starting Hercules in tmux session '$session'..."
echo "Working directory: $herc_dir"

# Create the session with the login window, start the login-server
tmux new-session -d -s $session -n login -c $herc_dir
tmux send-keys -t $session:login "./login-server" Enter

# Give login-server ~2s to start listening before char-server tries to connect
sleep 2

# Create char window
tmux new-window -t $session -n char -c $herc_dir
tmux send-keys -t $session:char "./char-server" Enter

# Give char-server ~2s to register with login-server
sleep 2

# Create map window
tmux new-window -t $session:2 -n map -c $herc_dir
tmux send-keys -t $session:map "./map-server" Enter

echo ""
echo "All three servers booting. Map-server takes ~30-60s to finish loading scripts."
echo ""
echo "  Attach:    tmux attach -t $session"
echo "  Switch:    Ctrl+B then 0/1/2  (login/char/map)"
echo "  Detach:    Ctrl+B then D"
echo "  Stop all:  ./scripts/stop.fish"

