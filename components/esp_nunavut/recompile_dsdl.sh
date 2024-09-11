#!/bin/bash

# This script downloads the latest OpenCyphal public regulated data type DSDLs from GitHub
# and compiles them to C headers in the 'nunavut/' directory.

# Run this script if you'd like to update the esp_nunavut component.

set -e

echo "Downloading public regulated DSDLs from GitHub."
wget https://github.com/OpenCyphal/public_regulated_data_types/archive/refs/heads/master.zip
unzip -qq -o master.zip
rm -f master.zip

echo
echo "Installing nunavut in temporary Python virtual environment..."
python -m venv tmp_venv
tmp_venv/bin/pip install --upgrade pip
tmp_venv/bin/pip install nunavut

echo
echo "Compiling DSDLs to C headers in 'nunavut/'."
mkdir -p nunavut
tmp_venv/bin/nnvg -O nunavut --target-language c public_regulated_data_types-master/uavcan
tmp_venv/bin/nnvg -O nunavut --target-language c public_regulated_data_types-master/reg --lookup-dir public_regulated_data_types-master/uavcan

rm -rf tmp_venv
rm -rf public_regulated_data_types-master
echo
echo "Success! The 'nunavut/' directory now contains the compiled C headers."
