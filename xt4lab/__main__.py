"""Command line entry point. No camera discovery or passthrough options."""
import argparse
from pathlib import Path
from .firmware import REPO, prepare_run
from .run import RunSession
from .report import write_report


def main():
    parser = argparse.ArgumentParser(description='Offline X-T4 firmware execution lab')
    commands = parser.add_subparsers(dest='command', required=True)
    report = commands.add_parser('report')
    report.add_argument('run_dir',type=Path)
    run = commands.add_parser('run')
    run.add_argument('--research-root',type=Path,required=True)
    run.add_argument('--out',type=Path,required=True)
    run.add_argument('--profile',choices=['module-entry-diagnostic','bootstrap-entry-diagnostic',
                                       'bootstrap-port-config-diagnostic',
                                       'bootstrap-input-exploration','bootstrap-timer-exploration',
                                       'bootstrap-controller-exploration','bootstrap-flash-exploration','bootstrap-phy-exploration','bootstrap-calibration-exploration','bootstrap-selector2c-exploration','bootstrap-gic-exploration','validation'],
                     default='module-entry-diagnostic')
    run.add_argument('--steps',type=int,default=1000)
    run.add_argument('--interactive',action='store_true')
    run.add_argument('--fast',action='store_true')
    run.add_argument('--scenario', choices=['zero-ports-v1', 'one-ports-v1'])
    args = parser.parse_args()
    if args.command == 'report':
        print(write_report(args.run_dir))
        return
    if args.fast and args.interactive:
        parser.error('--fast does not support --interactive')
    prepared = prepare_run(args.research_root.resolve(),args.out,args.profile,
                           scenario_name=args.scenario)
    session = RunSession(prepared, REPO/'build/qemu-system-arm',budget=args.steps,
                         execution_mode='bounded-continue' if args.fast else 'single-step')
    try:
        if not args.interactive:
            print(session.continue_fast() if args.fast else session.continue_steps(args.steps))
        else:
            print('Virtual firmware only. Commands: step, continue N, pause, reset, quit; Ctrl-C pauses.')
            while True:
                parts = input('xt4> ').split()
                if not parts:
                    continue
                if parts[0] == 'quit':
                    break
                if parts == ['step']:
                    print(session.step())
                elif parts[0] == 'continue':
                    print(session.continue_steps(int(parts[1]) if len(parts)>1 else args.steps))
                elif parts == ['pause']:
                    session.pause()
                elif parts == ['reset']:
                    session.reset()
                else:
                    print('Use step, continue N, pause, reset, or quit.')
    except (EOFError,KeyboardInterrupt):
        pass
    finally:
        session.close()
        print(write_report(prepared.directory))


if __name__ == '__main__':
    main()
