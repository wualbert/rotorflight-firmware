#!/usr/bin/env bash
set -euo pipefail
cd /work/sitl
make -j3 SITL ARM_SDK_DIR=/usr ARM_SDK_PREFIX= > /work/evidence/sitl-build.log 2>&1
make -j3 SITL ARM_SDK_DIR=/usr ARM_SDK_PREFIX= OPTIONS='SITL_GYRO_RATE_HZ=8000 SITL_REALTIME_CLOCK' OBJECT_DIR=./obj/denom-final > /work/evidence/denom-sitl-build.log 2>&1
run() {
    local name=$1; shift
    tools/sitl/run_all.sh -n "$name" -s journal -e /work/sitl/obj/main/rotorflight_SITL.elf "$@" > "/work/evidence/$name-run.log" 2>&1
    node /work/firmware/tools/validation/analyze.cjs "obj/sitl_runs/$name" --json "/work/evidence/$name.json" > "/work/evidence/$name-analysis.log" 2>&1
}
run full-between -- --between --toggles 12
run CHANGES -c 'set blackbox_params = CHANGES' -- --toggles 4
run OFF -c 'set blackbox_params = OFF'
run extended -- --extended
run header-write -- --header-write
for denom in 1 2 3 4 8; do
    SITL_GYRO_RATE_HZ=$((1000 * denom)) tools/sitl/run_all.sh -n "denom-$denom" -s journal -c "set pid_process_denom = $denom" -e /work/sitl/obj/denom-final/rotorflight_SITL.elf -- --step 0.5 --armed1 2 --armed2 1 --toggles 4 > "/work/evidence/denom-$denom-run.log" 2>&1
    node /work/firmware/tools/validation/analyze.cjs "obj/sitl_runs/denom-$denom" --json "/work/evidence/denom-$denom.json" > "/work/evidence/denom-$denom-analysis.log" 2>&1
done
bash /work/firmware/tools/validation/fault_matrix.sh
node /work/firmware/tools/validation/protocol_faults.cjs obj/sitl_runs/extended/blackbox.bbl > /work/evidence/protocol-faults.json
