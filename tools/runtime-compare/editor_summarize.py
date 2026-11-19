"""Validate and summarize existing raw export timings without rerunning either app."""
import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import statistics


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('report', type=Path)
    args = ap.parse_args()
    report = json.loads(args.report.read_text(encoding='utf-8-sig'))
    groups = defaultdict(list)
    signatures = defaultdict(set)
    for run in report['runs']:
        key = run['engine'], run['width'], run['height'], run['case']
        if len(run['ms']) != report['protocol']['samples_per_block']:
            raise ValueError(f'Wrong sample count {key}')
        output = args.report.parent/run['output']
        actual = hashlib.sha256(output.read_bytes()).hexdigest()
        if actual.lower() != run['output_sha256'].lower():
            raise ValueError(f'Output hash changed {output}')
        # Per-process JSON is original stdout and retains exact assembly/runtime IDs.
        raw = json.loads(output.with_suffix('.json').read_text(encoding='utf-8-sig'))
        if raw['ms'] != run['ms']:
            raise ValueError(f'Raw timing mismatch {output}')
        run['assembly_version'] = raw.get('assembly_version')
        run['runtime_version'] = raw.get('runtime_version')
        groups[key].extend(run['ms'])
        signatures[key].add(run['pixel_signature'])
    summary = []
    for (engine,w,h,case), values in sorted(groups.items()):
        ordered = sorted(values)
        if len(values) != report['protocol']['samples_per_block']*report['protocol']['blocks']:
            raise ValueError('Missing run block')
        key = engine,w,h,case
        summary.append(dict(engine=engine,case=case,width=w,height=h,n=len(values),
                            median_ms=statistics.median(values),p95_ms=ordered[math.ceil(len(values)*.95)-1],
                            max_ms=max(values),min_ms=min(values),stable_output_signature=len(signatures[key])==1))
    report['summary'] = summary
    report['summary_method'] = 'Median of all measured samples; nearest-rank p95. Recomputed from preserved raw stdout, all PNG hashes checked.'
    report['output_verification'] = {'run_count':len(report['runs']),'case_groups':len(summary),
                                     'timing_samples':sum(len(r['ms']) for r in report['runs']),
                                     'all_output_hashes_match':True,'all_group_signatures_stable':all(s['stable_output_signature'] for s in summary)}
    args.report.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'verification':report['output_verification'],'summary':summary},indent=2))


if __name__ == '__main__':
    main()
