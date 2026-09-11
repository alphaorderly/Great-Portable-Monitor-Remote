# Run with: source ./env.zsh
# Tools are isolated in this project; no shell startup files are modified.
export IDF_TOOLS_PATH="${${(%):-%x}:A:h:h}/.tools/espressif"
export IDF_PATH="${IDF_TOOLS_PATH:h}/esp-idf"
export IDF_PYTHON_ENV_PATH="$IDF_TOOLS_PATH/python_env/idf5.5_py3.12_env"
python3 "${IDF_TOOLS_PATH:h:h}/firmware/scripts/apply_nimble_patch.py" "$IDF_PATH" || return 1
source "$IDF_PATH/export.sh"
