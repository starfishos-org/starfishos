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

labels = ['Global', 'IPI', 'Hybrid Copy', 'Cap Tree', 'Cap Group', 'Thread', 'Connection', 'Notification', 'IRQ', 'PMO', 'VMSpace', 'WAIT MIGREATE', 'Alloc']
# colors = ['grey', '#BCCCA3', '#0072BD', '#8682BD', '#D96A73', '#FABC55', 'grey', '#BCCCA3', '#0072BD', '#8682BD', '#D96A73', '#FABC55']
hatches = ['', '|||', '\\\\\\', '///', '++', '\\/\\/\\/', '', '|||', '\\\\\\', '///', '++', '\\/\\/\\/']

# colors = ['#E64B35','#4DBBD6','#00A086','#3D5488']

workload_dict = {
    'Default': 'default',
    'SQLite': 'sqlite',
    'LevelDB':  'leveldb',
    'WordCount': 'word_count',
    'KMeans':'kmeans',
    'Redis': 'redis',
    'Memcached': 'memcached',
    # 'PCA': 'pca',
}

extra_workload_dict = {
    'PCA': 'pca',
    'KMeans':'kmeans',
    'Redis': 'redis',
    'Memcached': 'memcached',
}
