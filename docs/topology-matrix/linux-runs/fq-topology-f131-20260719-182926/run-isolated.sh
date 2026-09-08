#!/usr/bin/env bash
set -u
out=/tmp/fq-topology-f131-20260719-182926
cmd='cd /tmp/fq-topology-f131-20260719-182926/src && python3 tools/run_topology_matrix.py --max-cpus 0 --transfers 720720 --min-sample-ms 250 --rounds 12 --warmups 2 --3d-max-cpus 0 --out /tmp/fq-topology-f131-20260719-182926/artifacts'
governor=performance
rt_policy=rr
rt_priority=10
state="$out/isolation-effective.json"
restore="$out/governors-before.tsv"
python3 - "$state" "$governor" "$rt_policy" "$rt_priority" <<'PY'
import json, os, pathlib, sys, time
out, governor, policy, priority = sys.argv[1:]
cpus = pathlib.Path('/sys/devices/system/cpu')
def governors():
    return {p.parent.parent.name: p.read_text().strip() for p in cpus.glob('cpu*/cpufreq/scaling_governor')}
def run(cmd):
    import subprocess
    p=subprocess.run(cmd,text=True,capture_output=True)
    return {'command':cmd,'returncode':p.returncode,'stdout':p.stdout.strip(),'stderr':p.stderr.strip()}
data={'schema':1, 'requested':{'governor':governor,'rt_policy':policy,'rt_priority':int(priority)}, 'started_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'before_governor':governors(), 'launcher_affinity':run(['taskset','-pc',str(os.getpid())]), 'launcher_scheduler':run(['chrt','-p',str(os.getpid())]), 'actions':[]}
pathlib.Path(out).write_text(json.dumps(data, indent=2, sort_keys=True)+'\n')
PY
if [ "$governor" = performance ]; then
  sudo -n true || { echo 'performance governor needs passwordless sudo' >> "$out/run.log"; exit 77; }
  : > "$restore"
  for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
    [ -r "$f" ] || continue
    printf '%s\t%s\n' "$f" "$(cat "$f")" >> "$restore"
    printf performance | sudo -n tee "$f" >/dev/null
  done
fi
restore_governor() {
  [ -f "$restore" ] || return 0
  while IFS=$'\t' read -r f old; do printf '%s' "$old" | sudo -n tee "$f" >/dev/null || true; done < "$restore"
}
trap restore_governor EXIT
if [ "$rt_policy" != none ]; then
  case "$rt_priority" in ''|*[!0-9]*) exit 78;; esac
  [ "$rt_priority" -ge 1 ] && [ "$rt_priority" -le 99 ] || exit 78
  sudo -n true || { echo 'RT scheduling needs passwordless sudo' >> "$out/run.log"; exit 77; }
  policy_flag=-r; [ "$rt_policy" = fifo ] && policy_flag=-f
  sudo -n chrt "$policy_flag" "$rt_priority" bash -lc "$cmd"
else
  bash -lc "$cmd"
fi
rc=$?
python3 - "$state" "$rc" <<'PY'
import json, pathlib, subprocess, sys, time
p=pathlib.Path(sys.argv[1]); d=json.loads(p.read_text()); d['finished_utc']=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()); d['exit_code']=int(sys.argv[2])
def run(c):
 x=subprocess.run(c,text=True,capture_output=True); return {'command':c,'returncode':x.returncode,'stdout':x.stdout.strip(),'stderr':x.stderr.strip()}
d['effective_before_restore']={'governor':{x.parent.parent.name:x.read_text().strip() for x in pathlib.Path('/sys/devices/system/cpu').glob('cpu*/cpufreq/scaling_governor')},'launcher_scheduler':run(['chrt','-p',str(__import__('os').getpid())]),'launcher_affinity':run(['taskset','-pc',str(__import__('os').getpid())])}
p.write_text(json.dumps(d,indent=2,sort_keys=True)+'\n')
PY
exit "$rc"
