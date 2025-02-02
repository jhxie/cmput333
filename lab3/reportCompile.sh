#!/bin/bash

out="REPORT.pdf"
in0="a2sliding/REPORT.md"
in1="part1/REPORT.md"
in2="part2/REPORT.md"

if [[ -f "$in0" && -f "$in1" && -f "$in2" ]]; then
    # if 'pandoc' is not found
    if ! command -v pandoc > /dev/null 2>&1; then
        echo "[FAIL] pandoc not found"
    else
        cat "$in0" "$in1" "$in2" | pandoc -f markdown_github -o "$out"
        echo "[SUCCESS] The compiled result is generated."
    fi
else
    echo "[FAIL] Some of the input files are missing."
fi

