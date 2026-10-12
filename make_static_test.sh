#!/bin/bash
set -e

DIR=/tmp/bldd-static
rm -rf "$DIR"; mkdir -p "$DIR"

gcc -static -O0 -o "$DIR/static_hello" -x c - <<'EOF'
#include <stdio.h>
int main(void){ puts("static"); return 0; }
EOF

gcc -O0 -o "$DIR/dyn_hello" -x c - <<'EOF'
#include <stdio.h>
int main(void){ puts("dynamic"); return 0; }
EOF

gcc -shared -fPIC -O0 -o "$DIR/libfoo.so" -x c - <<'EOF'
int foo(void){ return 42; }
EOF

file "$DIR"/*
