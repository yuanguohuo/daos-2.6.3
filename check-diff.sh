#!/bin/bash

echo "all changes"
git diff 3f64a5454a52f4ee8e56ec22052136836e892ca0 ':!.gitignore' ':!ccls-linux' ':!ccls-macos' ':!check-diff.sh' ':!*/SConscript' \
    | grep ^[+-]                     \
    | egrep -v '^\+[[:space:]]*\/\/' \
    | egrep -v '^\+$'                \
    | egrep -v '^\-\-\- a/src'       \
    | egrep -v '^\+\+\+ b/src'

echo "new changes"
git diff \
    | grep ^[+-]                     \
    | egrep -v '^\+[[:space:]]*\/\/' \
    | egrep -v '^\-[[:space:]]*\/\/' \
    | egrep -v '^\+$'                \
    | egrep -v '^\-\-\- a/src'       \
    | egrep -v '^\+\+\+ b/src'
