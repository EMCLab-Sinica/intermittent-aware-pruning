import argparse
import logging
import os.path
import signal
import tempfile
import sys
from subprocess import Popen, TimeoutExpired

logging.basicConfig(level=logging.DEBUG)
logger = logging.getLogger('run-intermittently')

CHUNK_SIZE = 2000
CHUNK_LINES = 20

def run_one_inference(program, interval, logfile, shutdown_after_writes):
    first_run = True
    while True:
        cmd = [program, '1']
        if first_run and shutdown_after_writes:
            cmd.extend(['-c', str(shutdown_after_writes)])
        with Popen(cmd, stdout=logfile, stderr=logfile) as proc:
            try:
                kwargs = {}
                if not shutdown_after_writes:
                    kwargs['timeout'] = interval
                outs, errs = proc.communicate(**kwargs)
            except TimeoutExpired:
                proc.send_signal(signal.SIGINT)
            proc.wait()
            first_run = False
            if proc.returncode == 2:
                # simulated power failure
                continue
            if proc.returncode in (1, -signal.SIGFPE, -signal.SIGSEGV):
                logger.error('Program crashed!')
                sys.exit(1)
            if proc.returncode == 0:
                logfile.seek(0, 2)
                file_size = logfile.tell()
                logfile.seek(-(CHUNK_SIZE if file_size > CHUNK_SIZE else file_size), 2)
                last_chunk = logfile.read()
                print('\n'.join(last_chunk.decode('ascii').split('\n')[-CHUNK_LINES:]))
                return

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rounds', type=int, default=0)
    parser.add_argument('--interval', type=float, default=0.01)
    parser.add_argument('--shutdown-after-writes', type=int, default=0)
    parser.add_argument('program')
    args = parser.parse_args()

    logfile_path = os.path.join(tempfile.gettempdir(), 'intermittent-cnn')
    rounds = 0
    while True:
        with open(logfile_path, mode='w+b') as logfile:
            run_one_inference(args.program, args.interval, logfile, args.shutdown_after_writes)
            rounds += 1
            if args.rounds and rounds >= args.rounds:
                break


if __name__ == '__main__':
    main()
