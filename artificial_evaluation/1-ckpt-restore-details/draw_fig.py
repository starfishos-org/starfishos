import sys
import numpy as np
import matplotlib.pyplot as plt
import re
import os
import os.path as op
from break_down import parseFile, getTimeCount
from break_down_config import *
import seaborn as sns

args = sys.argv
if len(args) < 3:
    print("usage: python draw_fig.py [indir] [a/b]")
    sys.exit(1)
arg1 = args[1]
arg2 = args[2]

print("drawing fig", arg2, "...")

indir = arg1
# indir = "../ckpt-breakdown/"
allTimes = []
workloads = []
for label, fname in workload_dict.items():
    files = os.listdir(indir)
    # get all files that start with fname
    matches = [f for f in files if f.startswith(fname)]
    
    workloads.append(label)

    times = []
    for m in matches:
        print("parsing benchmark: ", label)
        _groups = parseFile(indir + m)
        if len(_groups) <= 2:
            print("data in file ", indir + m, " is incomplete, please re-run it!")
            print(_groups)
            exit(0)
        _times, _counts, _first_times, _ = getTimeCount(_groups)
        times.append(_times)

    avg_times = []
    for i in range(len(times[0])):
        total = 0
        for j in range(len(times)):
            total += times[j][i]
        avg = total / len(times)
        avg_times.append(avg)
    
    allTimes.append(avg_times)

# Draw figures
x = np.arange(0, len(workloads))

# traverse each era
plt.rcdefaults()
plt.rcParams.update({'font.size': 22, 'figure.figsize': (8, 4)})
plt.figure()
plt.rc('xtick', labelsize=18)
plt.xticks(x, workloads, rotation=20)
plt.rc('ytick', labelsize=18)
# plt.xlabel("Workloads", fontsize=16)
plt.ylabel("Checkpoint Time (μs)", fontsize=22)
plt.tight_layout()

if str(arg2) == "a":
    my_palette = sns.color_palette("RdGy",4)
    colors = [my_palette[i] for i in range(len(my_palette))]

    y = np.array(allTimes).T/1000.0

    y[OBJ] = sum(y[CAP_GROUP:VMSPACE+1])
    draw = [IPI, SYS, OBJ]
    draw_count = 0
    bottom = [0] * len(workloads)

    width = 0.25
    for i in draw:
        plt.bar(x - width/2, y[i], width, bottom = bottom, color=colors[draw_count], label=labels[i], edgecolor='black')
        draw_count += 1
        bottom += y[i]

    i = MIGREATE
    # plt.bar(x + width/2, y[i], width, color=colors[draw_count], label=labels[i], hatch=hatches[draw_count], edgecolor='black')  
    plt.bar(x + width/2, y[i], width, color=colors[draw_count], label=labels[i], edgecolor='black')      

    plt.grid(True, axis='y', linestyle=':')
    plt.legend(fontsize=18, frameon=False, ncol=2, loc='upper left', columnspacing=0.5)


elif str(arg2) == "b":
    my_palette = sns.color_palette("RdGy", 6)
    colors = [my_palette[i] for i in range(len(my_palette))]
    
    allTimes = np.array(allTimes).T/1000.0

    draw = [CAP_GROUP, THREAD, CONNECTION, NOTIFICATION, PMO, VMSPACE]
    draw_count = 0
    bottom = [0] * len(workloads)
    # print(workloads, allTimes)
    for i in draw:
        plt.bar(workloads, allTimes[i], 0.5, bottom = bottom, color=colors[draw_count], label=labels[i], edgecolor='black')
        draw_count += 1
        bottom += allTimes[i]

    plt.grid(True, axis='y', linestyle=':')
    plt.legend(fontsize=18, frameon=False, ncol=2, loc='upper left', columnspacing=0.5)

else:
    print("invalid args")

# plt.show()
path = './result/'
if not os.path.exists(path):
    os.mkdir(path)  
plt.savefig('./result/fig9{}.jpg'.format(arg2), format='jpg', dpi=1000)
