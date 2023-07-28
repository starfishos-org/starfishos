import sys
import numpy as np
import matplotlib.pyplot as plt
import re
import os
import os.path as op
from break_down_config import *
import pandas as pd

CAP_GROUP = 0
THREAD = 1
CONNECTION = 2
NOTIFICATION = 3
IRQ = 4
PMO = 5
VMSPACE = 6
TYPE_NR = 7

def parseFile(infile):
    with open(infile, 'r') as f:
        lines = f.readlines()

    # split lines into groups
    groups = []
    current = []
    counts = []

    on = False
    for line in lines:
        if line.find("[CKPT WS] latest") >= 0:
            on = True
            continue
        if line.find("tcnt:") >= 0:
            on = False
            current.append(line)
            groups.append(current)
            counts.append(len(current))
            current = []
            continue
        if on:
            current.append(line)

    # assert not on and np.max(counts) == np.min(counts)
    return groups

def find(lines, str):
    for line in lines:
        if line.find(str) >= 0:
            res = int(line.replace('\r', '').replace('\n', '').split()[-1])
            # print("find", str, res)
            return res
    print('Error: cannot find str "%s" in current group:\n' % str)
    for line in lines:
        print(line, end='')
    assert False

def findCounts(lines):
    counts = [0] * 7
    for line in lines:
        if line.find("object count") >= 0:
            line = line.replace('\r', '').replace('\n', '')
            m = re.match("^object count ([0-9]+): ([0-9]+), time: ([0-9]+) *", line)
            assert not m is None
            d = [int(x) for x in m.groups()]
            counts[d[0]] = d[1], d[2]
    return counts


def getTimeCount(groups):
    times = [0] * 7
    totalCounts = np.zeros((7, 2), dtype=np.float32)

    # ignore the first 5 
    for i, group in enumerate(groups):
        counts = findCounts(group)
        totalCounts += np.array(counts)

        times[CAP_GROUP] += find(group, "object count 0")
        times[THREAD] += find(group, "object count 1")
        times[CONNECTION] += find(group, "object count 2")
        times[NOTIFICATION] += find(group, "object count 3")
        times[IRQ] += find(group, "object count 4")
        times[PMO] += find(group, "object count 5")
        times[VMSPACE] += find(group, "object count 6")
    return times, totalCounts

if __name__ == '__main__':
    # "/restore-breakdown/"
    indir = sys.argv[1]

    # for root, dirs, files in os.walk(dir_path):
    #     for file_name in files:
    #         file_path = os.path.join(root, file_name)
    #         print(file_path)
    #         printinfo(file_path)
    # incr_res = {}
    restore_res ={}
    for label, fname in workload_dict.items():
        files = os.listdir(indir)
        # print(files)
        # get all files that start with fname
        matches = [f for f in files if f.startswith(fname)]
        
        for m in matches:
            _groups = parseFile(indir + m)
            _times, _counts = getTimeCount(_groups)
            for i in range(CAP_GROUP, VMSPACE + 1):
                if _counts[i-CAP_GROUP][0] == 0:
                    continue
                _times[i] = float(_times[i])/_counts[i-CAP_GROUP][0]

            restore_res[label] = _times

    # # incr-th, full-th, 
    df = pd.DataFrame.from_dict(restore_res).transpose()
    c={0: 'C.G.', 1: 'Thread', 2: 'IPC', 3: 'Noti.', 4: 'IRQ', 5: 'PMO', 6: 'VMS'}
    df = df.rename(columns=c)
    print("Table3 (Restore):")
    print(df)
    df.to_csv("./result/table3-restore-column.csv")
    # df = pd.DataFrame.from_dict(full_res).transpose()
    # # print(wls)
    # # df.insert(0, threads, wls)
    # df.to_csv("obj-detail-full.csv")