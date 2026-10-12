#!/usr/bin/env bash
set -e

DIR=/tmp/bldd-multiarch
rm -rf "$DIR"; mkdir -p "$DIR" "$DIR/.libs32"

gcc -shared -fPIC -O0 -Wl,-soname,libfoo.so \
    -o "$DIR/libfoo.so" -x c - <<'EOF'
int foo(void){ return 42; }
EOF

gcc -O0 -o "$DIR/uses_foo64" -x c - \
    -L"$DIR" -lfoo -Wl,-rpath,"$DIR" <<'EOF'
int foo(void);
int main(void){ return foo(); }
EOF

if gcc -m32 -shared -fPIC -O0 -Wl,-soname,libfoo.so \
       -o "$DIR/.libs32/libfoo.so" -x c - 2>/dev/null <<'EOF'
int foo(void){ return 42; }
EOF
then
    gcc -m32 -O0 -o "$DIR/uses_foo32" -x c - \
        -L"$DIR/.libs32" -lfoo -Wl,-rpath,"$DIR/.libs32" <<'EOF'
int foo(void);
int main(void){ return foo(); }
EOF
else
    echo "warning: gcc -m32 unavailable, -o section will be empty" >&2
    rm -rf "$DIR/.libs32"
fi

file "$DIR"/* "$DIR"/.libs32/* 2>/dev/null || true
