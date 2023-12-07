import sys

x = 1
for i in range(1, int(sys.argv[1]) + 1):
    x = i * x
print(hex(x))
