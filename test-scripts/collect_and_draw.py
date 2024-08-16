import re
import matplotlib.pyplot as plt

threads = [1, 2, 4, 8, 16, 32, 64]
cases = ["all_cxl", "pgtable_dram_only", "heap_cxl_only", "shared_data_cxl_only"]
result = {}

base_dir = "/home/yjs/chcore-cxl"

for case in cases:
  result[case] = []
  for thread in threads:
    file_path = f'{base_dir}/log/{thread}/{case}.out'

    with open(file_path, 'r') as file:
      log_content = file.read()

      # 使用正则表达式提取 'library:' 后的数字
      match = re.search(r'library:\s*(\d+)', log_content)
      if match:
          library_number = int(match.group(1))
          result[case].append(library_number) 
      else:
          print("No 'library:' number found.")


plt.figure(figsize=(10, 6))

for label, values in result.items():
    plt.plot(threads[:len(values)], values, marker='o', label=label)

plt.title('Data Visualization')
plt.xlabel('Threads')
plt.ylabel('Time(ns)')
plt.xticks(threads)
plt.grid()
plt.legend()


plt.savefig('test_data_visualization.png', dpi=300, bbox_inches='tight')
