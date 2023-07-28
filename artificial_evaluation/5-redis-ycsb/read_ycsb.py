import os
import re
import pandas as pd
import os, sys

ROOT_DIR=sys.argv[1]
# search_base='../logs/treesls/ycsb/'
# search_dirs = ['chcore-baseline', 'chcore-ckpt1ms', 'linux-baseline', 'nvm-log', 'disk-log']
search_dirs = []
items = os.listdir(ROOT_DIR)
for item in items:
    item_path = os.path.join(ROOT_DIR, item)
    if os.path.isdir(item_path):
        search_dirs.append(item)

filename_pattern = r'(\w+)\..+\.(\w+)\.(\d+)\.log'
result = {}

for d in search_dirs:
    d = ROOT_DIR + '/' + d
    print(d)
    files = [f for f in os.listdir(d) if re.match(filename_pattern, f)]
    for f in files:
        filepath = os.path.join(d, f)
        match = re.match(filename_pattern, f)
        workload, threads, run = match.groups()
        # print(workload, threads, run)
        with open(filepath) as logfile:
            contents = logfile.read()
            match = re.search(r'(\d+(\.\d+)?)\s*$', contents)
            if match:
                value = float(match.group(1))

                label = d.split('/')[-1]
                if threads not in result:
                    result[threads] = {}
                if label not in result[threads]:
                    result[threads][label] = {}
                if workload not in result[threads][label]:
                    result[threads][label][workload] = value
                else:
                    result[threads][label][workload] = (result[threads][label][workload]  + value)/2

# print(result)
for threads, labels in result.items():
    # wls = []
    # for label, workloads in labels.items():
    #     for workload, value in workloads.items():
    #         row = {
    #             'label': label,
    #             'workload': workload,
    #             'value': value
    #         }
    #         wls.append(workload)
    # df = pd.DataFrame(workloads, columns=['label', 'workload', 'value'])
    df = pd.DataFrame.from_dict(labels)
    # print(wls)
    # df.insert(0, threads, wls)
    df.to_csv("./result/ycsb.csv".format(threads))