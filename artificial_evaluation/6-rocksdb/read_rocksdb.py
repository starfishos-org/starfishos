import progbg as sb
import progbg.graphing as g
import pandas as pd
import os, sys

ROOT_DIR=sys.argv[1]
# ROOT_DIR='../logs/treesls/rocksdb/'

fs = []
items = os.listdir(ROOT_DIR)
for item in items:
    item_path = os.path.join(ROOT_DIR, item)
    if os.path.isdir(item_path):
        fs.append(item)
# fs = ["chcore-base", "chcore-ckpt"]
result = {}

def parse(label, path):
    files = [f for f in os.listdir(path)]
    for file in files:
        filepath = os.path.join(path, file)
        with open(filepath) as f:
            lines = f.readlines()
            found = False
            for i, l in enumerate(lines):
                if "Microseconds per write" in l:
                    if found:
                        # get "P50" and "P99"
                        for ll in lines[i+1:i+7]:
                            if "Average" in ll:
                                avg = float(ll.strip().split()[3])
                            if "P99" in ll:
                                nine = float(ll.strip().split()[6])
                                ninenine = float(ll.strip().split()[8])
                                break
                        # add results to dict
                        if label not in result:
                            result[label] = {}
                        if "P50" not in result[label]:
                            result[label]["P50"] = avg
                        else:
                            result[label]["P50"] = (result[label]["P50"] + avg)/2
                        if "P99" not in result[label]:
                            result[label]["P99"] = nine
                        else:
                            result[label]["P99"] = (result[label]["P99"] + nine)/2
                    else:
                        found = True
                if "mixgraph" in l and "ops/sec" in l:
                    thp = int(l.strip().split()[4])
                    if label not in result:
                        result[label] = {}
                    if "Throughput" not in result[label]:
                        result[label]["Throughput"] = thp
                    else:
                        result[label]["Throughput"] = (result[label]["Throughput"] + avg)/2
            if found == False:
                print("[Error] {} did not execute or pre-emptively shut down, please re-run fig14.sh".format(path))
                os.unlink(path)

    
for f in fs:
    path = os.path.join(ROOT_DIR, f)
    parse(f, path)

df = pd.DataFrame.from_dict(result)
df.to_csv("./result/rocksdb.csv")
