#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <cmath>

using namespace std;
using namespace std::chrono;

struct BenchmarkResult {
    double mflops;
    double latency;
};

vector<vector<double>> createRandomMatrix(int n) {
    random_device rd;
    mt19937 gen(rd());
    uniform_real_distribution<> dis(-0.5, 0.5);
    
    vector<vector<double>> A(n, vector<double>(n));
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            A[i][j] = dis(gen);
        }
    }
    return A;
}

vector<double> sumRows(const vector<vector<double>>& A) {
    int n = A.size();
    vector<double> B(n, 0.0);
    
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            B[i] += A[i][j];
        }
    }
    return B;
}

// Simple Gaussian elimination solver for Ax = B
vector<double> solveLinearSystem(vector<vector<double>> A, vector<double> B) {
    int n = B.size();
    
    // Forward elimination
    for (int i = 0; i < n; ++i) {
        // Search for maximum in this column
        double maxEl = abs(A[i][i]);
        int maxRow = i;
        for (int k = i+1; k < n; ++k) {
            if (abs(A[k][i]) > maxEl) {
                maxEl = abs(A[k][i]);
                maxRow = k;
            }
        }
        
        // Swap maximum row with current row
        swap(A[maxRow], A[i]);
        swap(B[maxRow], B[i]);
        
        // Make all rows below this one 0 in current column
        for (int k = i+1; k < n; ++k) {
            double c = -A[k][i]/A[i][i];
            for (int j = i; j < n; ++j) {
                if (i == j) {
                    A[k][j] = 0;
                } else {
                    A[k][j] += c * A[i][j];
                }
            }
            B[k] += c * B[i];
        }
    }
    
    // Back substitution
    vector<double> x(n);
    for (int i = n-1; i >= 0; --i) {
        x[i] = B[i]/A[i][i];
        for (int k = i-1; k >= 0; --k) {
            B[k] -= A[k][i] * x[i];
        }
    }
    return x;
}

BenchmarkResult linpack(int n) {
    // LINPACK benchmarks
    double ops = (2.0 * n) * n * n / 3.0 + (2.0 * n) * n;
    
    // Create AxA array of random numbers -0.5 to 0.5
    auto A = createRandomMatrix(n);
    auto B = sumRows(A);
    
    // Solve Ax = B
    auto start = steady_clock::now();
    auto x = solveLinearSystem(A, B);
    auto stop = steady_clock::now();
    
    double latency = duration_cast<duration<double>>(stop - start).count();
    double mflops = (ops * 1e-6 / latency);
    
    return {mflops, latency};
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        cerr << "Usage: " << argv[0] << " <matrix_size> <global_memory_malloc_type>" << endl;
        return 1;
    }
    
    int n = stoi(argv[1]);
    extern int global_memory_malloc_type;
    global_memory_malloc_type = stoi(argv[2]);

    std::cout << "Running " << argv[0] << std::endl;
    cout << "n: " << n << endl;
    cout << "global_memory_malloc_type: " << global_memory_malloc_type << endl;
    auto result = linpack(n);
    
    cout << "MFLOPS: " << result.mflops << endl;
    cout << "Time: " << result.latency << " seconds" << endl;
    cout << "done" << endl;
    
    return 0;
}