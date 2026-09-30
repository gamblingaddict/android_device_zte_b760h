#!/bin/bash
#
# Copyright (C) 2016 The CyanogenMod Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -e

VENDOR=zte
DEVICE=b760h

# Load extractutils and do some sanity checks
MY_DIR="${BASH_SOURCE%/*}"
if [[ ! -d "$MY_DIR" ]]; then MY_DIR="$PWD"; fi

CM_ROOT="$MY_DIR"/../../..

HELPER="$CM_ROOT"/vendor/cm/build/tools/extract_utils.sh
if [ ! -f "$HELPER" ]; then
    echo "Unable to find helper script at $HELPER"
    exit 1
fi
. "$HELPER"

if [ $# -eq 0 ]; then
  SRC=adb
else
  if [ $# -eq 1 ]; then
    SRC=$1
  else
    echo "$0: bad number of arguments"
    echo ""
    echo "usage: $0 [PATH_TO_EXPANDED_ROM]"
    echo ""
    echo "If PATH_TO_EXPANDED_ROM is not specified, blobs will be extracted from"
    echo "the device using adb pull."
    exit 1
  fi
fi

# Initialize the helper
setup_vendor "$DEVICE" "$VENDOR" "$CM_ROOT"

extract "$MY_DIR"/configs/helpers/proprietary-files.txt "$SRC"

DEVICE_BLOB_ROOT="${CM_ROOT}"/vendor/"${VENDOR}"/"${DEVICE}"/proprietary

patchelf --add-needed "libshim_audio.so" "${DEVICE_BLOB_ROOT}"/lib/libaudio.primary.default.so
patchelf --add-needed "libshim_log.so" "${DEVICE_BLOB_ROOT}"/lib/libaudio.primary.default.so

patchelf --add-needed "libshim_omx.so" "${DEVICE_BLOB_ROOT}"/lib/libMtkOmxVdec.so
patchelf --add-needed "libshim_omx.so" "${DEVICE_BLOB_ROOT}"/lib/libMtkOmxVenc.so
patchelf --add-needed "libshim_ui.so" "${DEVICE_BLOB_ROOT}"/lib/libMtkOmxVenc.so

"$MY_DIR"/configs/helpers/setup-makefiles.sh
