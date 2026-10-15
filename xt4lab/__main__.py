"""Command line entry point. No camera discovery or passthrough options."""
import argparse
from pathlib import Path
from .firmware import REPO, prepare_run
from .run import RunSession


def main():
    parser = argparse.ArgumentParser(description='Offline X-T4 firmware execution lab')
    commands = parser.add_subparsers(dest='command', required=True)
    run = commands.add_parser('run')
    run.add_argument('--research-root',type=Path,required=True)
    run.add_argument('--out',type=Path,required=True)
    run.add_argument('--profile',choices=['module-entry-diagnostic','validation'],default='module-entry-diagnostic')
    run.add_argument('--steps',type=int,default=1000)
    run.add_argument('--interactive',action='store_true')
    args = parser.parse_args()
    prepared = prepare_run(args.research_root,args.out,args.profile)
    session = RunSession(prepared, REPO/'build/qemu-system-arm',budget=args.steps)
    try:
        if not args.interactive:
            print(session.continue_steps(args.steps))
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


if __name__ == '__main__':
    main()
