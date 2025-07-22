import os
import re
import csv
import glob

def extract_values(file_path):
    with open(file_path, 'r') as f:
        content = f.read()
        
    results = {}
    
    # prepare阶段的指标
    prepare_patterns = {
        'PREPARE': r'perf_cfork_time\[PREPARE\]:\s*(\d+)',
        'PREPARE_KVS': r'perf_cfork_time\[PREPARE_KVS\]:\s*(\d+)',
        'CKPT': r'perf_cfork_time\[CKPT\]:\s*(\d+)',
        'CKPT_STOP_ALL_THREADS': r'perf_cfork_time\[CKPT_STOP_ALL_THREADS\]:\s*(\d+)',
        'CKPT_THREADS': r'perf_cfork_time\[CKPT_THREADS\]:\s*(\d+)',
        'CKPT_CAP_GROUP': r'perf_cfork_time\[CKPT_CAP_GROUP\]:\s*(\d+)',
        'CKPT_OTHER': r'perf_cfork_time\[CKPT_OTHER\]:\s*(\d+)'
    }
    
    # restore阶段的指标
    restore_patterns = {
        'RESTORE': r'perf_restore_time\[RESTORE\]:\s*(\d+)',
        'RESTORE_KVS': r'perf_restore_time\[RESTORE_KVS\]:\s*(\d+)',
        'RESTORE_PROMOTE_THREADS': r'perf_restore_time\[RESTORE_PROMOTE_THREADS\]:\s*(\d+)',
        'RESTORE_START_ALL_THREADS': r'perf_restore_time\[RESTORE_START_ALL_THREADS\]:\s*(\d+)'
    }
    
    # prepare copy time指标
    copy_patterns = {
        'copy_cap_group': r'prepare copy time object: cap group,\s*(\d+)',
        'copy_thread': r'prepare copy time object: thread,\s*(\d+)',
        'copy_connection': r'prepare copy time object: connection,\s*(\d+)',
        'copy_notification': r'prepare copy time object: notification,\s*(\d+)',
        'copy_irq_notification': r'prepare copy time object: irq notification,\s*(\d+)',
        'copy_pmobject': r'prepare copy time object: pmobject,\s*(\d+)',
        'copy_vmspace': r'prepare copy time object: vmspace,\s*(\d+)'
    }
    
    # 根据文件类型选择要使用的模式
    if 'prepare' in file_path:
        patterns = {**prepare_patterns, **copy_patterns}
    else:  # restore文件
        patterns = restore_patterns
    
    # 提取所有值
    for key, pattern in patterns.items():
        match = re.search(pattern, content)
        if match:
            results[key] = int(match.group(1))
        else:
            results[key] = None
            
    return results

def process_logs(log_dir):
    data = {}
    log_files = glob.glob(os.path.join(log_dir, "cfork_*.log"))
    
    for file_path in log_files:
        base_name = os.path.basename(file_path)
        benchmark = base_name.split('_')[1].split('.')[0].upper()
        is_prepare = 'prepare' in file_path
        
        if benchmark not in data:
            data[benchmark] = {
                'prepare': {},
                'restore': {}
            }
        
        values = extract_values(file_path)
        
        if is_prepare:
            data[benchmark]['prepare'] = values
        else:
            data[benchmark]['restore'] = values

    # 获取所有可能的指标名称
    prepare_metrics = set()
    restore_metrics = set()
    for benchmark_data in data.values():
        prepare_metrics.update(benchmark_data['prepare'].keys())
        restore_metrics.update(benchmark_data['restore'].keys())
    
    # 写入CSV文件
    with open('results_detailed.csv', 'w', newline='') as f:
        writer = csv.writer(f)
        
        # 构建表头
        header = ['Benchmark']
        # 先添加prepare阶段的所有指标
        for metric in sorted(prepare_metrics):
            header.append(f'{metric}')
        # 再添加restore阶段的所有指标
        for metric in sorted(restore_metrics):
            header.append(f'{metric}')
        writer.writerow(header)
        
        # 写入数据
        for benchmark in sorted(data.keys()):
            row = [benchmark]
            # 添加prepare阶段的数据
            for metric in sorted(prepare_metrics):
                row.append(data[benchmark]['prepare'].get(metric))
            # 添加restore阶段的数据
            for metric in sorted(restore_metrics):
                row.append(data[benchmark]['restore'].get(metric))
            writer.writerow(row)

    print("详细数据已成功导出到 results_detailed.csv")

# 使用脚本
log_dir = "./ckpt-logs-new"  # 当前目录，您可以修改为实际的日志文件目录
process_logs(log_dir)