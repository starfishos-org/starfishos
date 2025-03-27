#include <iostream>
#include <vector>
#include <ctime>
#include <cstdlib>
#include <cmath>
#include <chrono>

using namespace std;

typedef vector<vector<double>> Matrix;
typedef vector<double> Vector;

// Function to multiply two matrices A and B
Matrix multiply(const Matrix& A, const Matrix& B, int n) {
    Matrix C(n, Vector(n, 0.0));
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            for (int k = 0; k < n; ++k) {
                C[i][j] += A[i][k] * B[k][j];
            }
        }
    }
    return C;
}

// Function to perform Gaussian Elimination to solve Ax = B
Vector gaussianElimination(const Matrix& A, const Vector& B, int n) {
    Matrix augmented(n, Vector(n + 1, 0.0));
    Vector X(n, 0.0);
    
    // Create augmented matrix [A | B]
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            augmented[i][j] = A[i][j];
        }
        augmented[i][n] = B[i];
    }

    // Perform Gaussian elimination
    for (int i = 0; i < n; ++i) {
        // Find the pivot
        double maxEl = abs(augmented[i][i]);
        int maxRow = i;
        for (int k = i + 1; k < n; ++k) {
            if (abs(augmented[k][i]) > maxEl) {
                maxEl = abs(augmented[k][i]);
                maxRow = k;
            }
        }

        // Swap rows
        if (maxRow != i) {
            swap(augmented[i], augmented[maxRow]);
        }

        // Eliminate column below the pivot
        for (int j = i + 1; j < n; ++j) {
            double coeff = augmented[j][i] / augmented[i][i];
            for (int k = i; k < n + 1; ++k) {
                augmented[j][k] -= augmented[i][k] * coeff;
            }
        }
    }

    // Back substitution
    for (int i = n - 1; i >= 0; --i) {
        X[i] = augmented[i][n] / augmented[i][i];
        for (int j = i - 1; j >= 0; --j) {
            augmented[j][n] -= augmented[j][i] * X[i];
        }
    }

    return X;
}

// Function to compute LINPACK benchmark
struct BenchmarkResult {
    double mflops;
    double latency;
};

BenchmarkResult linpack(int n) {
    // Number of operations
    double ops = (2.0 * n) * n * n / 3.0 + (2.0 * n) * n;

    // Create AxA matrix with random values between -0.5 and 0.5
    Matrix A(n, Vector(n, 0.0));
    Vector B(n, 0.0);
    srand(time(0)); // Seed random number generator

    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            A[i][j] = ((rand() % 1000) / 1000.0) - 0.5; // Random value between -0.5 and 0.5
        }
        B[i] = 0.0;
        for (int j = 0; j < n; ++j) {
            B[i] += A[i][j];
        }
    }

    // Start timing the solution process
    auto start = chrono::steady_clock::now();
    Vector x = gaussianElimination(A, B, n);
    auto end = chrono::steady_clock::now();
    double latency = chrono::duration_cast<chrono::duration<double>>(end - start).count() * 1000;

    // Calculate Mflops
    double mflops = (ops * 1e-6) / latency;

    // Return result
    BenchmarkResult result = {mflops, latency};
    return result;
}

int main(int argc, char** argv) {
    int n = 1000;
    if (argc > 1) {
        n = atoi(argv[1]);
    }

    cout << "Running LINPACK benchmark with matrix size " << n << "x" << n << endl;

    BenchmarkResult result = linpack(n);

    cout << "Mflops: " << result.mflops << endl;
    cout << "Latency: " << result.latency << " seconds" << endl;

    return 0;
}
