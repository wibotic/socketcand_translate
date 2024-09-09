#!/bin/bash

# This script compiles the dsdl files in `public_regulated_data_types/uavcan/`
# and `public_regulated_data_types/reg/` to C headers in `nunavut/`.

set -e

echo "Installing nunavut in temporary virtual environment..."
python -m venv tmp_venv
tmp_venv/bin/pip install --upgrade pip
tmp_venv/bin/pip install nunavut

echo
echo "Compiling DSDLs in 'public_regulated_data_types/uavcan/'"
echo "and 'public_regulated_data_types/reg/' to"
echo "C headers in 'nunavut/'."
mkdir -p nunavut
tmp_venv/bin/nnvg -O nunavut --target-language c public_regulated_data_types/uavcan
tmp_venv/bin/nnvg -O nunavut --target-language c public_regulated_data_types/reg --lookup-dir public_regulated_data_types/uavcan

rm -rf tmp_venv
echo
echo "Success! The 'nunavut/' directory now contains the compiled C headers."
