"""Report observed execution without promoting hypotheses to camera fidelity."""
import hashlib
import json
from pathlib import Path
from .firmware import REPO, read_regular
from .run import write_json


def write_report(run_dir: Path) -> Path:
    run_dir = run_dir.absolute()
    if run_dir != run_dir.resolve() or not run_dir.is_relative_to(REPO/'runs'):
        raise ValueError('Report must refer to a regular project run')
    info = json.loads(read_regular(run_dir,'run-info.json'))
    engine = json.loads(read_regular(run_dir,'engine-build.json'))
    events = [json.loads(line) for line in read_regular(run_dir,'trace.jsonl').decode().splitlines() if line]
    starts = [e for e in events if e['event']=='start']
    segment = starts[-1]['segment'] if starts else -1
    pcs = [e['pc'] for e in events if e['event']=='instruction' and e['segment']==segment]
    stop = {'reason':'incomplete','segment':segment,'completed_steps':len(pcs)}
    if (run_dir/'stop.json').exists():
        saved = json.loads(read_regular(run_dir,'stop.json'))
        if saved.get('segment') == segment:
            stop = saved
    canonical = [{k:v for k,v in event.items() if k not in ('argv','engine_events')}
                 for event in events]
    digest = hashlib.sha256(json.dumps(canonical,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    dependencies = []
    reset_states = []
    for file in sorted((run_dir/'segments').glob('*/engine-events.jsonl')):
        relative = file.relative_to(run_dir)
        for number,line in enumerate(read_regular(run_dir,str(relative)).decode().splitlines(),1):
            record = json.loads(line)
            if record.get('event')=='reset':
                reset_states.append({'evidence':str(relative)+':'+str(number),
                                     'registers':record['registers']})
            if record.get('event')=='stop':
                dependencies.append({
                    'pc':record['pc'], 'address':record.get('physical_address')
                        if record.get('physical_address') is not None else record.get('virtual_address'),
                    'access':record['access'], 'width':record.get('width'),
                    'evidence':str(relative)+':'+str(number),
                    'confidence':'Observed in emulation under explicit diagnostic assumptions',
                    'unresolved_question':'Which real loader state, mapping, or device supplies this dependency?'
                })
    summary = {'schema_version':1,'camera_booted':False,
        'profile':info['profile']['name'], 'assumptions':info['profile']['assumptions'],
        'original_sha256':info['provenance']['original_sha256'],
        'profile_sha256':info['provenance']['profile_sha256'],
        'engine_sha256':engine['binary_sha256'],'engine_source_base':engine['source_base'],
        'completed_steps':len(pcs),'completed_pcs':pcs,'stop':stop,
        'interrupted_attempts':sum(e['event']=='interrupted-step' for e in events),
        'starting_states':reset_states,'canonical_trace':canonical,
        'canonical_trace_sha256':digest,'dependencies':dependencies,
        'evidence':['trace.jsonl','run-info.json','engine-build.json','segments/']}
    write_json(run_dir/'summary.json',summary)
    write_json(run_dir/'dependencies.json',dependencies)
    lines = ['# X-T4 offline firmware execution report','',
        f"Completed **{len(pcs)} original firmware instructions** in the latest segment.",
        f"Stop: **{stop['reason']}**.",'',
        f"Profile: `{info['profile']['name']}`. This run uses the recorded diagnostic starting state. "
        'Full camera boot, menus and SD support remain unimplemented. No physical camera was accessed.','',
        '## Observed stop','']
    if summary['interrupted_attempts']:
        lines[5:5] = ['An interrupted step has uncertain completion; the count includes only confirmed single-step completions.', '']
    for key in ['pc','physical_address','virtual_address','width','access','value','attempted_steps','completed_steps']:
        if key in stop:
            value = stop[key]
            if key in ['pc','physical_address','virtual_address','value'] and isinstance(value,int):
                value = f'0x{value:08x}'
            lines.append(f'- {key}: `{value}`')
    lines += ['', '## Assumptions', '', *('- '+a for a in summary['assumptions']), '',
        '## Reproducibility', '', f"- Original DAT SHA256: `{summary['original_sha256']}`",
        f"- Engine SHA256: `{summary['engine_sha256']}`",
        f"- Canonical trace SHA256: `{digest}`",'',
        'Input fingerprints and startup profile: [run-info.json](run-info.json). '
        'Build provenance: [engine-build.json](engine-build.json). '
        'Instruction evidence: [trace.jsonl](trace.jsonl). '
        'Next dependencies: [dependencies.json](dependencies.json).','']
    output = run_dir/'report.md'
    # JSON reports are authoritative; Markdown is a readable local rendering.
    output.write_text('\n'.join(lines))
    return output
