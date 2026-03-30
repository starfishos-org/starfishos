#!/usr/bin/env python3
"""
Configure timing flags for IPC benchmarks

Usage:
  python3 configure_timing.py --breakdown 0 --srv-timing 0
  python3 configure_timing.py --breakdown 1 --srv-timing 0
  python3 configure_timing.py --breakdown 1 --srv-timing 1
"""

import re
import sys
import argparse
from pathlib import Path

def set_define(filepath, define_name, value):
    """Set a #define value in a C header file"""
    with open(filepath, 'r') as f:
        content = f.read()

    # Match: #define ENABLE_BREAKDOWN 0
    pattern = rf'(#define\s+{define_name}\s+)\d+'
    replacement = rf'\g<1>{value}'

    new_content = re.sub(pattern, replacement, content)

    if new_content == content:
        print(f"✗ Flag {define_name} not found in {filepath}")
        return False

    with open(filepath, 'w') as f:
        f.write(new_content)

    print(f"✓ Set {define_name}={value} in {filepath}")
    return True

def main():
    parser = argparse.ArgumentParser(description='Configure IPC benchmark timing flags')
    parser.add_argument('--breakdown', type=int, default=None, help='ENABLE_BREAKDOWN (0 or 1)')
    parser.add_argument('--srv-timing', type=int, default=None, help='ENABLE_SRV_TIMING (0 or 1)')
    parser.add_argument('--list', action='store_true', help='List current flag values')

    args = parser.parse_args()

    repo_root = Path(__file__).parent.parent.parent

    client_file = repo_root / 'user/system-servers/polling/polling_client_test.c'
    server_file = repo_root / 'user/system-servers/polling/polling_server.c'
    resp_file = repo_root / 'user/system-servers/polling/polling_resp.c'

    if args.list:
        print("Current timing configuration:")
        print()

        for filepath, flag_name in [
            (client_file, 'ENABLE_BREAKDOWN'),
            (server_file, 'ENABLE_SRV_TIMING'),
            (resp_file, 'ENABLE_SRV_TIMING'),
        ]:
            with open(filepath, 'r') as f:
                for line in f:
                    if f'#define {flag_name}' in line:
                        print(f"  {filepath.name:30} {line.strip()}")
                        break
        return

    if args.breakdown is not None:
        if args.breakdown not in (0, 1):
            print("Error: --breakdown must be 0 or 1")
            sys.exit(1)
        set_define(client_file, 'ENABLE_BREAKDOWN', args.breakdown)

    if args.srv_timing is not None:
        if args.srv_timing not in (0, 1):
            print("Error: --srv-timing must be 0 or 1")
            sys.exit(1)
        set_define(server_file, 'ENABLE_SRV_TIMING', args.srv_timing)
        set_define(resp_file, 'ENABLE_SRV_TIMING', args.srv_timing)

    if args.breakdown is None and args.srv_timing is None and not args.list:
        print("No configuration specified. Use --breakdown and/or --srv-timing")
        print("Run with --list to see current values")
        sys.exit(1)

if __name__ == '__main__':
    main()
