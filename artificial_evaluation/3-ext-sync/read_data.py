import os, sys
import pandas as pd

# Define the directory path
directory = sys.argv[1]
mode = sys.argv[2]

# Define the prefixes and metrics
prefixes = ['ckpt0.', 'ckpt1.', 'ckpt5.', 'ckpt10.']
metrics = ['P50', 'Throughput']

# Initialize the dictionary
data = {}
data['raw'] = {}
data['cal'] = {}

# Loop through the log files
for prefix in prefixes:
    data['raw'][prefix] = {}
    data['cal'][prefix] = {}

    # Loop through the metrics
    for metric in metrics:
        data['raw'][prefix][metric] = []
        data['cal'][prefix][metric] = 0

    # Find log files with the given prefix
    file_names = [file for file in os.listdir(directory) if file.startswith(prefix)]

    # Loop through the log files
    for file_name in file_names:
        file_path = os.path.join(directory, file_name)
        
        # Read the log file
        with open(file_path, 'r') as file:
            lines = file.readlines()
        
        # Parse the log file and extract the required metrics
        for l in lines:
            if "50% <=" in l:
                data['raw'][prefix]['P50'].append(float(l.split()[2]))
            if "requests per second" in l:
                data['raw'][prefix]['Throughput'].append(float(l.split()[0]))
# print(data)

for prefix in prefixes:
    for metric in metrics:
        length = len(data['raw'][prefix][metric])
        if length != 0:
            data['cal'][prefix][metric] = sum(data['raw'][prefix][metric])/length

    # Convert the dictionary to a DataFrame
    df = pd.DataFrame.from_dict(data['cal'])
    # Save the DataFrame as a CSV file
    df.to_csv('./result/ext-sync-{}.csv'.format(mode))
    print(df)
