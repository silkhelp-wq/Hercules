#!/usr/bin/env fish
# Tail all three Hercules log files in one view.
# Usage: ./scripts/tail-logs.fish

set -l herc_dir (realpath (dirname (status -f))/..)
cd $herc_dir

set -l logs log/login-server.log log/char-server.log log/map-server.log

# Verify logs exist
for f in $logs
    if not test -e $f
        echo "Warning: $f does not exist yet (server may not have been started)"
    end
end

echo "Tailing all three server logs. Ctrl+C to stop watching."
echo "================================================================="

tail -f $logs

