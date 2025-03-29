#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>

extern int memory_malloc_type;

double float_operation(int N) {
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < N; i++) {
    double sin_i = std::sin(i);
    double cos_i = std::cos(i);
    double sqrt_i = std::sqrt(i);
    // Prevent compiler from optimizing out the calculations
    static volatile double sink;  // Alternative way to prevent optimization
    sink = sin_i;
    sink = cos_i;
    sink = sqrt_i;
    (void)sink;
  }
  auto end = std::chrono::steady_clock::now();
  std::chrono::duration<double> duration = end - start;
  return duration.count();
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: " << argv[0] << " <N> <memory_malloc_type>\n";
    return 1;
  }

  int N;
  try {
    N = std::stoi(argv[1]);
  } catch (const std::exception& e) {
    std::cerr << "Invalid input for N: " << e.what() << '\n';
    return 1;
  }
  try {
    memory_malloc_type = std::stoi(argv[2]);
  } catch (const std::exception& e) {
    std::cerr << "Invalid input for memory_malloc_type: " << e.what() << '\n';
    return 1;
  }

  if (N <= 0) {
    std::cerr << "N must be greater than 0\n";
    return 1;
  }
  if (memory_malloc_type != 0 && memory_malloc_type != 1 && memory_malloc_type != 2) {
    std::cerr << "memory_malloc_type must be 0 or 1 or 2\n";
    return 1;
  }

  std::cout << "Running " << argv[0] << std::endl;
  std::cout << "N: " << N << '\n';
  std::cout << "memory_malloc_type: " << memory_malloc_type << '\n';

  double latency = float_operation(N);
  std::cout << "Time: " << latency << " seconds\n";
  std::cout << "done\n";

  return 0;
}