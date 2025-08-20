#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <atomic>
#include <sched.h>

using namespace std;

// bind thread to core
void bind_thread_to_core(int core_id) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core_id, &mask);
    pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
}

using Matrix = vector<vector<double>>;

// 初始化矩阵
void init_matrix(Matrix &mat, int size, double value = 1.0) {
    mat.resize(size, vector<double>(size, value));
}

// 线程函数：计算部分行的矩阵乘法
void multiply_partial(int core_id, const Matrix &A, const Matrix &B, Matrix &C, int start_row, int end_row, int size, atomic<double> &total_time) {
    bind_thread_to_core(core_id);

    auto start = chrono::steady_clock::now();
    
    for (int i = start_row; i < end_row; ++i) {
        for (int j = 0; j < size; ++j) {
            double sum = 0.0;
            for (int k = 0; k < size; ++k) {
                sum += A[i][k] * B[k][j];
            }
            C[i][j] = sum;
        }
    }
    
    auto end = chrono::steady_clock::now();
    chrono::duration<double> duration = end - start;
    total_time = total_time + duration.count();
}

// 计算GFLOPS
double compute_gflops(int size, double seconds) {
    // 矩阵乘法操作次数：2 * n^3
    double operations = 2.0 * size * size * size;
    return (operations / 1e9) / seconds;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        cerr << "usage: " << argv[0] << " matrix_size num_threads\n";
        return 1;
    }

    int size = atoi(argv[1]);
    int num_threads = atoi(argv[2]);

    if (size <= 0 || num_threads <= 0) {
        cerr << "matrix_size and num_threads must be positive integers.\n";
        return 1;
    }

    Matrix A, B, C;
    init_matrix(A, size, 1.0);
    init_matrix(B, size, 1.0);
    init_matrix(C, size, 0.0);

    vector<thread> threads;
    int rows_per_thread = size / num_threads;
    atomic<double> total_time(0.0);
    bind_thread_to_core(0);

    for (int t = 0; t < num_threads; ++t) {
        int start_row = t * rows_per_thread;
        int end_row = (t == num_threads - 1) ? size : start_row + rows_per_thread;
        threads.emplace_back(multiply_partial, t, cref(A), cref(B), ref(C), start_row, end_row, size, ref(total_time));
    }

    for (auto &t : threads) {
        t.join();
    }

    double avg_time = total_time / num_threads;
    double gflops = compute_gflops(size, avg_time);

    cout << "matrix_size: " << size << "x" << size << ", num_threads: " << num_threads << endl;
    cout << "time: " << avg_time << " seconds (average per thread)" << endl;
    cout << "total thread time: " << total_time << " seconds" << endl;
    cout << "gflops: " << gflops << " GFLOPS" << endl;

    return 0;
}
