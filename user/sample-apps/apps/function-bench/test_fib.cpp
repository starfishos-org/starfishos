#include <iostream>
#include <chrono>
#include <cstdlib>

// 递归计算斐波那契数列
long long fibonacci(int n) {
    if (n <= 1) return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

// 迭代计算斐波那契数列
long long fibonacci_iterative(int n) {
    if (n <= 1) return n;
    
    long long a = 0, b = 1, c;
    for (int i = 2; i <= n; i++) {
        c = a + b;
        a = b;
        b = c;
    }
    return b;
}

int main(int argc, char* argv[]) {
    int n = 30; // 默认值
    bool use_recursive = false;
    
    // 解析命令行参数
    if (argc > 1) {
        n = std::atoi(argv[1]);
    }
    
    if (argc > 2) {
        use_recursive = std::atoi(argv[2]) != 0;
    }
    
    std::cout << "calculate " << n << "\n";
    std::cout << (use_recursive ? "recursive" : "iterative") << " method" << std::endl;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    long long result;
    if (use_recursive) {
        result = fibonacci(n);
    } else {
        result = fibonacci_iterative(n);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "result: " << result << std::endl;
    std::cout << "time: " << duration.count() << " us" << std::endl;
    
    return 0;
}
