#!/bin/bash

if [ $# -ne 1 ]; then
    echo "Usage: ./ab_server.sh {path to file of tags}"
    exit 1
fi

tag_format='--tag='
declare -a input_tags
command=('./ab_server.exe' '--debug' '--plc=ControlLogix' '--path=1,0')

while read -r line || [[ -n "$line" ]]; do
    line_mod=$(echo $line | tr -d '\r')
    if ! [[ "$line_mod" =~ ^[a-zA-Z0-9_]+":"[A-Z]+"["[0-9]+"]"$ ]]; then
        echo "Wrong TAG: $line"
        echo "Exit Program"
        exit 1
    else
        echo "TAG: $line"
    fi
    one_tag=$(echo "$tag_format$line" | tr -d '\r')
    input_tags+=($one_tag)
done < $1

echo "Run Commnad: ${command[@]} ${input_tags[@]}"

OUTPUT_FILE="./command.txt"
"${command[@]}" "${input_tags[@]}"
