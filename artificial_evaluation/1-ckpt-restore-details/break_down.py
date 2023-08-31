import sys
import numpy as np
import matplotlib.pyplot as plt
import re
import os
import os.path as op
from break_down_config import *
import pandas as pd

SYS = 0
IPI = 1
MIGREATE = 2
OBJ = 3
CAP_GROUP = 4
THREAD = 5
CONNECTION = 6
NOTIFICATION = 7
IRQ = 8
PMO = 9
VMSPACE = 10
ALLOC = 11
MIGREATE_WAIT = 12
TYPE_NR = 13

PFCOUNT = 0
DIRTY_PAGE = 1
TOT_CACHED_PAGE = 2
EXTRA_TYPE_NR = 3

vars = ['System_Vars', 'IPI', 'MIGREATE', 'OBJ', 'CAP_GROUP', 'THREAD', 'CONNECTION', 'NOTIFICATION', 'IRQ', 'PMO', 'VMSPACE', 'KVS', 'MEMCPY', 'ALLOC', 'THRACK_ACCESS', 'PTE_POLL']
labels = ['System Vars', 'IPI', 'Reset Page Table', 'K-V Store', 'Kernel Malloc', 'Data Copy']
# colors = ['orange', 'c', 'red', 'green', 'yellow', 'brown']
colors = ['grey', '#BCCCA3', '#0072BD', '#8682BD', '#D96A73', '#FABC55']
hatches = ['', '|||', '\\\\\\', '///', '++', '\\/\\/\\/']

def calculate_median(data):
    sorted_data = sorted(data)
    n = len(sorted_data)
    if n == 0:
        return 0
    if n % 2 == 0:
        median = (sorted_data[n//2 - 1] + sorted_data[n//2]) / 2
    else:
        median = sorted_data[n//2]
    # median = sorted_data[n*9//10]
    return median

def parseFile(infile):
    with open(infile, 'r') as f:
        lines = f.readlines()

    # split lines into groups
    groups = []
    current = []
    counts = []

    on = False
    for line in lines:
        if line.find("==LOG") >= 0:
            on = True
            continue
        # if line.find("==END") >= 0:
        if line.find("active list") >= 0:
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

# handle one era
def find(lines, str):
    for line in lines:
        if line.find(str) >= 0:
            res = int(line.replace('\r', '').replace('\n', '').split()[-1])
            # print("find", str, res)
            return res
    # return 0
    print('Error: cannot find str "%s" in current group:\n' % str)
    for line in lines:
        print(line, end='')
    assert False

def findMaxMigrateTime(lines):
    time_list = []
    for line in lines:
        if line.find('migrate time') >= 0:
            cpu_list = line.split()[2:]
            if len(cpu_list) == 1:
                return int(cpu_list[0])
            for i in range(len(cpu_list)//2):
                time_list.append(int(cpu_list[i*2+1]))
            break
    return max(time_list)    

def findExtraInfo(lines):
    extra = [0] * EXTRA_TYPE_NR
    for line in lines:
        if line.find('pf_count') >= 0:
            # get pf_count
            pf_count_start = line.find("pf_count=") + len("pf_count=")
            pf_count_end = line.find(",", pf_count_start)
            extra[PFCOUNT] = int(line[pf_count_start:pf_count_end])
            # break
        if line.find('active list') >= 0:
            num_regex = r"\d+"
            matches = re.findall(num_regex, line)
            if len(matches) >= 4:
                extra[DIRTY_PAGE] = int(matches[0])
                extra[TOT_CACHED_PAGE] = int(matches[1])
    # print(extra)
    return extra

def findCounts(lines):
    counts = [0] * 7
    for line in lines:
        if line.find("object count") >= 0:
            line = line.replace('\r', '').replace('\n', '')
            m = re.match("^object count ([0-9]+): ([0-9]+) ([0-9]+) *", line)
            assert not m is None
            d = [int(x) for x in m.groups()]
            counts[d[0]] = d[1], d[2]
    return counts

IGNORE_CNT = 1
MAX_CNT = 100 * 1000
def getTimeCount(groups):
    # traverse each group to get times
    times = [[] for _ in range(TYPE_NR)]
    first_times = [0] * TYPE_NR
    totalExtra = np.zeros(EXTRA_TYPE_NR, dtype=np.float32)
    totalCounts = np.zeros((7, 2), dtype=np.float32)
    length = len(groups) - IGNORE_CNT
    # ignore the first 5 
    for i, group in enumerate(groups):
        if i < IGNORE_CNT:
            try:
                first_times[CAP_GROUP] += find(group, "object count 0")
                first_times[THREAD] += find(group, "object count 1")
                first_times[CONNECTION] += find(group, "object count 2")
                first_times[NOTIFICATION] += find(group, "object count 3")
                first_times[IRQ] += find(group, "object count 4")
                first_times[PMO] += find(group, "object count 5")
                first_times[VMSPACE] += find(group, "object count 6")
                continue
            except (ValueError, AssertionError) as e:
                print(e)
                print(group, ": error in this group, please check")
                exit(0)
        else:
            try:
                times[IPI].append(find(group, "ipi time"))
                times[ALLOC].append(find(group, "get second latest obj"))
                times[OBJ].append(find(group, "ckpt object"))
                # times[KVS] += find(group, "kvs get time")
                # times[KVS] += find(group, "kvs put time") + find(group, "kvs get time")
                times[SYS].append(find(group, "recycle cost") + find(group, "fmap cost"))
                # times[MEMCPY] += find(group, "memcpy time")
                times[CAP_GROUP].append(find(group, "object count 0"))
                times[THREAD].append(find(group, "object count 1"))
                times[CONNECTION].append(find(group, "object count 2"))
                times[NOTIFICATION].append(find(group, "object count 3"))
                times[IRQ].append(find(group, "object count 4"))
                times[PMO].append(find(group, "object count 5"))
                times[VMSPACE].append(find(group, "object count 6"))
                # times[PTE_POLL] += find(group, "pte pool time")
                # times[THRACK_ACCESS] += find(group, "track access time")
                times[MIGREATE].append(findMaxMigrateTime(group))
                # times[MIGREATE_WAIT].append(find(group, "wait for migrate finish time"))
                extra = findExtraInfo(group)
                totalExtra += np.array(extra)
                counts = findCounts(group)
                totalCounts += np.array(counts)
            except (ValueError, AssertionError) as e:
                print(e)
                # exit(0)
                length = length - 1
                continue
    # print(len(groups), np.array(times, dtype=np.float32))
    # times = np.array(times, dtype=np.float32) / length
    _times = [0] * TYPE_NR
    for i in range(TYPE_NR):
        _times[i] = calculate_median(times[i])
    first_times = np.array(first_times, dtype=np.float32) / IGNORE_CNT
    totalCounts /= (length - 1)
    totalExtra /= (length - 1)
    return _times, totalCounts, first_times, totalExtra


# traverse each era
def printinfo(infile):
    allTimes = []
    allCounts = []
    # infile = sys.argv[1]
    groups = parseFile(infile)
    times, counts, first_times, extras = getTimeCount(groups)
    print(counts, extras)
    print("object time (ns)")
    for i in range(CAP_GROUP, VMSPACE + 1):
        if counts[i-CAP_GROUP][0] == 0:
            continue
        print(float(times[i])/counts[i-CAP_GROUP][0])
    print("object first time (ns)")
    for i in range(CAP_GROUP, VMSPACE + 1):
        if counts[i-CAP_GROUP][0] == 0:
            continue
        print(float(first_times[i])/counts[i-CAP_GROUP][0])
    times = times / 1000
    # with open(outProportion, 'w+') as ofile:
    for i in range(TYPE_NR):
        print(vars[i], ", ", times[i])
            # ofile.write("{}, {}\n".format(vars[i], times[i]))

if __name__ == '__main__':
    # indir = "../ckpt-breakdown-backup/"
    args = sys.argv
    if len(args) < 3:
        print("usage: python break_down.py [indir] [ckpt/extra/count]")
        sys.exit(1)
    indir = args[1]
    arg2 = args[2]

    c={0: 'C.G.', 1: 'Thread', 2: 'IPC', 3: 'Noti.', 4: 'IRQ', 5: 'PMO', 6: 'VMS'}

    path = './result/'
    if not os.path.exists(path):
        os.mkdir(path)

    if arg2 == 'ckpt':
        incr_res = {}
        full_res ={}
        # extra = {}
        for label, fname in workload_dict.items():
            files = os.listdir(indir)
            # get all files that start with fname
            matches = [f for f in files if f.startswith(fname)]
            
            __times = [0] * 7
            __first_times = [0] * 7

            for m in matches:
                _groups = parseFile(indir + m)
                _times, _counts, _first_times, _extra = getTimeCount(_groups)
                for i in range(CAP_GROUP, VMSPACE + 1):
                    if _counts[i-CAP_GROUP][0] == 0:
                        continue
                    __times[i-CAP_GROUP] = float(_times[i])/_counts[i-CAP_GROUP][0]
                    __first_times[i-CAP_GROUP] = float(_first_times[i])/_counts[i-CAP_GROUP][0]

                incr_res[label] = __times
                full_res[label] = __first_times
        
        # c={0: 'C.G.', 1: 'Thread', 2: 'IPC', 3: 'Noti.', 4: 'IRQ', 5: 'PMO', 6: 'VMS'}
        # incr-th, full-th, 
        df = pd.DataFrame.from_dict(incr_res).transpose()
        df = df.rename(columns=c)
        print("Table3 (Incr):")
        print(df)
        for column_name, column_data in df.items():
            ma = round(df.max()[column_name]/1000, 2)
            mi = round(df.min()[column_name]/1000, 2)
            print(column_name, "\tMin:", mi, "\tMax:", ma)
        df.to_csv("./result/table3-incur-colum.csv")

        df = pd.DataFrame.from_dict(full_res).transpose()
        df = df.rename(columns=c)
        print("Table3 (Full):")
        print(df)
        for column_name, column_data in df.items():
            ma = round(df.max()[column_name]/1000, 2)
            mi = round(df.min()[column_name]/1000, 2)
            print(column_name, "\tMin:", mi, "\tMax:", ma)
        df.to_csv("./result/table3-full-colum.csv")

    elif arg2 == 'extra':
        extra = {}
        for label, fname in extra_workload_dict.items():
            files = os.listdir(indir)
            # get all files that start with fname
            matches = [f for f in files if f.startswith(fname)]
            
            for m in matches:
                _groups = parseFile(indir + m)
                _, _, _, _extra = getTimeCount(_groups)
                extra[label] = _extra
            # PFCOUNT = 0
            # DIRTY_PAGE = 1
            # TOT_CACHED_PAGE = 2
        df = pd.DataFrame.from_dict(extra).transpose()
        df = df.rename(columns={0: '# of runtime page faults', 1: '# of dirty cached pages', 2: '# of cached pages'})
        print("Table4:")
        print(df)
        df.to_csv("./result/table4.csv")

    elif arg2 == 'count':
        count_res = {}

        for label, fname in workload_dict.items():
            files = os.listdir(indir)
            # get all files that start with fname
            matches = [f for f in files if f.startswith(fname)]
            __counts = [0] * 7

            for m in matches:
                _groups = parseFile(indir + m)
                _, _counts, _, _ = getTimeCount(_groups)
                for i in range(7):
                    __counts[i] = round(_counts[i][0])
                count_res[label] = __counts

        # counts 
        df = pd.DataFrame.from_dict(count_res).transpose()
        df = df.rename(columns=c)
        print("Table2 (Object Count):")
        print(df)
        df.to_csv("./result/table2-counts.csv")
